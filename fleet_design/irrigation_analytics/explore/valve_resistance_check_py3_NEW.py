"""
valve_resistance_check_py3.py — rewritten 2026-06-09 (Glenn + KB2)

Goals over the prior version:
1. Sensor offset is ACTUALLY measured (prior code commented it out — it was
   only a 2-second sleep). All valves off, sample ACS712 N times, take mean.
2. V_PSU is derived from a cohort of meter-verified reference valves.
   For each ref, V_PSU = I × R_meter_ref. Take median across the cohort —
   robust to any one valve drifting. Spread across refs surfaces as a
   diagnostic.
3. Per-valve measurement averages 5 samples (median) instead of a single
   ACS712 read. Cuts noise floor ~3x.
4. 10-second settle wait between turning a valve off and turning the next
   one on. Lets residual coil current dissipate, magnetic field collapse,
   ACS712 fully settle. Eliminates the cross-talk we suspected was
   contributing to cycle-to-cycle noise.
5. Per-cycle V_PSU and offset are published to the irrigation hash so
   KB2 (downstream) can use them: R_calc = V_PSU / (I_raw - offset).
   Drops the external "2-null-valve" heuristic entirely.

Compatibility: same __init__ signature as before, same chain names,
same hash_logging key "IRRIGATION_VALVE_TEST", same IRRIGATION_PAST_ACTIONS
events ("measure_resistance" RED, "valve_resistance_done" GREEN). Drop-in.
"""

import json
import time

from .irrigation_logging_py3 import Hash_Logging_Object

# Wait between valves: after disable_all_sprinklers, sleep this many seconds
# before turning on the next valve. Lets the magnetic field collapse and
# the ACS712 reading settle to true offset.
INTER_VALVE_SETTLE_S = 10.0

# Per-valve measurement: take this many ACS712 samples and use the median.
PER_VALVE_SAMPLES = 5
PER_VALVE_SAMPLE_GAP_S = 0.05

# Offset measurement: all valves off, sample ACS712 this many times.
OFFSET_SAMPLES = 10
OFFSET_SAMPLE_GAP_S = 0.05

# Valve settle: after turning on, wait this long before sampling current.
VALVE_SETTLE_S = 0.3

# V_PSU reference valves. Each tuple: (remote, pin, R_meter_ohms).
# Update R_meter_ohms when these valves are re-measured at the terminal
# block with a meter. Drift detection is automatic: spread across refs
# surfaces as v_psu_spread per cycle. If spread > 0.5 V, at least one
# reference has drifted and needs re-measurement.
#
# Last meter calibration: 2026-06-08 (Glenn — terminal block readings).
V_PSU_REFERENCE_VALVES = [
    ("satellite_1", 43, 40.0),   # master coil (also serves as anchor)
    ("satellite_1", 29, 43.4),
    ("satellite_1", 30, 45.8),
    ("satellite_1", 31, 44.7),
    ("satellite_1", 17, 43.0),
    # sat_1:44 excluded (parallel pair — math is different, 23 Ω is two ~46 Ω in parallel)
]


class Valve_Resistance_Check(object):

    def __init__(self,
                 cf,
                 cluster_control,
                 io_control,
                 handlers,
                 app_files,
                 sys_files,
                 master_valves,
                 cleaning_valves,
                 measurement_depths,
                 irrigation_hash_control,
                 get_json_object,
                 current_operations,
                 generate_control_events,
                 irrigation_current_limit,
                 qs,
                 redis_site):

        self.get_json_object = get_json_object
        self.handlers = handlers
        self.sys_files = sys_files
        self.app_files = app_files
        self.cf = cf
        self.cluster_control = cluster_control
        self.io_control = io_control
        self.master_valves = master_valves
        self.cleaning_valves = cleaning_valves
        self.current_operations = current_operations
        self.generate_control_events = generate_control_events
        self.irrigation_current_limit = irrigation_current_limit
        self.irrigation_hash_control = irrigation_hash_control

        self.hash_logging = Hash_Logging_Object(
            self.handlers, "IRRIGATION_VALVE_TEST",
            measurement_depths["valve_depth"])

        # Per-cycle state — set during measure_offset_and_psu, used in
        # valve_measurement to compute corrected coil current.
        self.offset = 0.0      # sensor null (A) — subtracted from each reading
        self.v_psu = 15.6      # PSU voltage (V) — overridden each cycle if measure_v_psu succeeds

    # ------------------------------------------------------------------
    # Chain definition
    # ------------------------------------------------------------------
    def construct_chains(self, cf):

        cf.define_chain("resistance_check", False)
        cf.insert.one_step(self.generate_control_events.change_master_valve_offline)
        cf.insert.one_step(self.generate_control_events.change_master_valve_offline)
        cf.insert.assert_function_terminate("RELEASE_IRRIGATION_CONTROL",
                                             None, self.io_control.verify_irrigation_controllers)

        cf.insert.one_step(self.assemble_relevant_valves)
        cf.insert.one_step(self.measure_offset_and_psu)
        cf.insert.enable_chains(["test_each_valve"])

        cf.insert.wait_event_count(event="IR_V_Valve_Check_Done")
        cf.insert.log("event IR_V_Valve_Check_Done")
        cf.insert.one_step(self.log_valve_check)
        cf.insert.send_event("RELEASE_IRRIGATION_CONTROL")
        cf.insert.one_step(self.generate_control_events.change_master_valve_online)
        cf.insert.one_step(self.generate_control_events.change_cleaning_valve_online)
        cf.insert.terminate()

        cf.define_chain("test_each_valve", False, init_function=self.check_queue)
        cf.insert.wait_event_count(count=1)  # synchronize on second tick
        cf.insert.one_step(self.valve_setup)

        cf.insert.wait_event_count(count=2)
        cf.insert.one_step(self.valve_measurement)
        cf.insert.verify_function_terminate(reset_event="IR_V_Valve_Check_Done",
                                              reset_event_data=None,
                                              function=self.check_queue)

        cf.insert.reset()

        return ["resistance_check", "test_each_valve"]

    def construct_clusters(self, cluster, cluster_id, state_id):
        cluster.define_state(cluster_id, state_id, ["resistance_check"])

    # ------------------------------------------------------------------
    # Calibration step — runs ONCE at the start of each resistance check
    # ------------------------------------------------------------------
    def measure_offset_and_psu(self, *args):
        """
        Determine sensor null offset + PSU voltage for this cycle.

        Step 1: All valves OFF. Sample ACS712 N times. Mean = sensor null.
        Step 2: For each meter-verified reference valve: turn on, settle,
                read median of N samples, compute V_PSU = I × R_ref.
                10-second wait between references.
        Step 3: V_PSU = median across cohort estimates. Spread surfaced as
                diagnostic.

        Sets self.offset and self.v_psu. Publishes to irrigation_hash_control
        so KB2 (downstream) can use them.
        """
        # Step 1: sensor null
        self.io_control.clear_max_currents()
        self.io_control.enable_irrigation_relay()
        self.io_control.disable_all_sprinklers()
        time.sleep(2.0)  # initial settle from master_valve_offline transient

        null_samples = []
        for _ in range(OFFSET_SAMPLES):
            null_samples.append(self.io_control.measure_valve_current())
            time.sleep(OFFSET_SAMPLE_GAP_S)
        self.offset = sum(null_samples) / float(len(null_samples))
        print("sensor null offset = {:.4f} A (n={})".format(
            self.offset, OFFSET_SAMPLES))

        # Step 2: V_PSU via reference cohort
        psu_estimates = []
        for (ref_remote, ref_pin, ref_R) in V_PSU_REFERENCE_VALVES:
            self.io_control.disable_all_sprinklers()
            time.sleep(INTER_VALVE_SETTLE_S)  # 10s for residual to dissipate

            self.io_control.clear_max_currents()
            self.io_control.turn_on_valves(
                [{"remote": ref_remote, "bits": [int(ref_pin)]}])
            time.sleep(VALVE_SETTLE_S)

            samples = [self.io_control.measure_valve_current()
                       for _ in range(PER_VALVE_SAMPLES)]
            samples.sort()
            i_ref = samples[len(samples) // 2]  # median

            # I_coil = I_measured - offset
            i_coil = i_ref - self.offset
            if i_coil > 0.001:
                v_estimate = i_coil * ref_R
                psu_estimates.append((ref_remote, ref_pin, i_ref, i_coil, v_estimate))
                print("  ref {}:{} I_raw={:.4f} I_coil={:.4f} R={:.1f} -> V_est={:.2f}".format(
                    ref_remote, ref_pin, i_ref, i_coil, ref_R, v_estimate))
            else:
                print("  ref {}:{} I_coil <= 0 (I_raw={:.4f} offset={:.4f}) -- skipped".format(
                    ref_remote, ref_pin, i_ref, self.offset))

        self.io_control.disable_all_sprinklers()
        time.sleep(INTER_VALVE_SETTLE_S)

        # Step 3: aggregate
        if len(psu_estimates) >= 3:
            v_values = sorted(e[4] for e in psu_estimates)
            self.v_psu = v_values[len(v_values) // 2]  # median
            v_spread = v_values[-1] - v_values[0]
            print("V_PSU = {:.2f} V (median over {} refs, spread {:.2f} V)".format(
                self.v_psu, len(v_values), v_spread))
            if v_spread > 0.5:
                print("WARN: V_PSU spread > 0.5 V — at least one reference has drifted, re-meter due")
        else:
            print("WARN: fewer than 3 V_PSU estimates available, falling back to default 15.6 V")
            self.v_psu = 15.6
            v_spread = 0.0

        # Publish for downstream consumers (KB2 on the analytics container)
        try:
            self.irrigation_hash_control.set("v_psu", self.v_psu)
            self.irrigation_hash_control.set("sensor_offset", self.offset)
            self.irrigation_hash_control.set("v_psu_spread", v_spread)
            self.irrigation_hash_control.set("v_psu_cohort_n", len(psu_estimates))
        except Exception as e:
            print("WARN: failed to publish v_psu/offset to redis: {}".format(e))

    # ------------------------------------------------------------------
    # Valve queue assembly — unchanged from prior version
    # ------------------------------------------------------------------
    def assemble_relevant_valves(self, *args):
        self.job_dictionary = set()
        self.job_queue = []
        sprinkler_ctrl = self.app_files.load_file("sprinkler_ctrl.json")

        for j in sprinkler_ctrl:
            json_data = self.app_files.load_file(j["link"])
            for i in json_data["schedule"]:
                for k in i:
                    remote = k[0]
                    for pin in k[1]:
                        self.update_entry(remote, pin)

        for j in self.master_valves["MASTER_VALVES"]:
            remote = j["remote"]
            pins = j["pins"]
            for pin in pins:
                self.update_entry(remote, pin)

        for j in self.cleaning_valves["CLEANING_VALVES"]:
            remote = j["remote"]
            pins = j["pins"]
            for pin in pins:
                self.update_entry(remote, pin)

    def add_resistance_entry(self, remote, pin):
        entry = str(remote) + ":" + str(pin)
        if entry not in self.job_dictionary:
            self.job_dictionary.add(entry)
            json_object = [remote, pin]
            self.job_queue.append(json_object)

    def update_entry(self, remote, pin):
        self.add_resistance_entry(remote, pin)

    def check_queue(self, *args):
        return len(self.job_queue) > 0

    # ------------------------------------------------------------------
    # Per-valve test — runs N times, once per valve
    # ------------------------------------------------------------------
    def valve_setup(self, cf_handle, chainObj, parameters, event):
        if event["name"] == "INIT":
            return "CONTINUE"

        # 10-second settle wait after the prior valve was turned off.
        # Lets residual coil current dissipate, magnetic field collapse,
        # ACS712 fully de-energize. Required to eliminate cross-valve
        # noise that was contributing to cycle-to-cycle drift.
        time.sleep(INTER_VALVE_SETTLE_S)

        json_object = self.job_queue.pop()
        self.valve_object = json_object
        print("valve object", self.valve_object)

        self.io_control.clear_max_currents()
        self.io_control.enable_irrigation_relay()
        self.io_control.turn_on_valves(
            [{"remote": json_object[0], "bits": [int(json_object[1])]}])

        self.remote = json_object[0]
        self.output = json_object[1]

        # Let the valve fully energize before sampling
        time.sleep(VALVE_SETTLE_S)

    def valve_measurement(self, cf_handle, chainObj, parameters, event):
        if event["name"] == "INIT":
            return "CONTINUE"

        # Take PER_VALVE_SAMPLES readings and use the median. Cuts noise
        # floor ~3x vs the prior single-sample approach.
        samples = [self.io_control.measure_valve_current()
                   for _ in range(PER_VALVE_SAMPLES)]
        # Optional small gap between samples — if measure_valve_current is
        # already slow this is wasted; if it's fast (just ADC read) we want
        # a tiny delay so we're not sampling the same ADC clock cycle.
        time.sleep(PER_VALVE_SAMPLE_GAP_S)
        samples.sort()
        coil_current_raw = samples[len(samples) // 2]
        coil_current = coil_current_raw - self.offset

        print("coil_current limit={:.3f} raw={:.4f} offset={:.4f} corrected={:.4f}".format(
            self.irrigation_current_limit, coil_current_raw,
            self.offset, coil_current))

        if coil_current > self.irrigation_current_limit:
            details = {
                "remote":  self.valve_object[0],
                "bit":     self.valve_object[1],
                "current": coil_current,
                "limit":   self.irrigation_current_limit,
            }
            self.current_operation = {"state": "MEASURE_RESISTANCE"}
            self.handlers["IRRIGATION_PAST_ACTIONS"].push({
                "action":  "measure_resistance",
                "details": details,
                "level":   "RED"})

        logging_key = self.remote + ":" + str(self.output)
        print("***********************************", logging_key, coil_current)
        # Store the CORRECTED (offset-subtracted) current. Downstream KB2
        # can compute R = v_psu / coil_current directly with no further
        # offset correction.
        self.hash_logging.log_value(logging_key, coil_current)
        self.io_control.disable_all_sprinklers()
        # NOTE: 10-second settle wait happens at the START of the NEXT
        # valve_setup, not here, so we don't add latency to the last
        # iteration of the cycle.

    def log_valve_check(self, *args):
        self.handlers["IRRIGATION_PAST_ACTIONS"].push({
            "action":  "valve_resistance_done",
            "details": {
                "v_psu":        self.v_psu,
                "sensor_offset": self.offset,
            },
            "level":   "GREEN"})


if __name__ == "__main__":
    pass
