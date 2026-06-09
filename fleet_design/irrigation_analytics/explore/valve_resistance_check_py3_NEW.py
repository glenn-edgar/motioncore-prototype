"""
valve_resistance_check_py3.py — rewritten 2026-06-09 PM (Glenn locked spec)

Minimal-change fix for the three real bugs in the prior version:
  1. measure_offset() was a no-op (2-second sleep + commented-out work).
  2. No inter-valve settle — back-to-back valves contaminated each other.
  3. Single-sample current reading — noise floor too high.

Algorithm (locked 2026-06-09 PM):

  1. All valves off, wait 3 seconds, take 5 ACS712 samples → offset = median.
  2. For each valve in the queue:
       a. All valves off.
       b. Wait 3 seconds. Flyback diodes on the terminal blocks clamp
          inductive kickback fast (typically <10 ms with proper diode),
          so 3 s is plenty to let coil current dissipate and the ACS712
          fully de-energize. Was 10 s in the cohort draft; Glenn reduced
          it once the flyback path was confirmed.
       c. Turn the valve on, brief energize settle.
       d. Take 5 ACS712 samples, median, subtract offset.
       e. Store in IRRIGATION_VALVE_TEST under "remote:pin" key.

  3. Emit IRRIGATION_PAST_ACTIONS "valve_resistance_done" event with the
     sensor_offset in details (for diagnostic, no consumer change needed).

Median (not mean) filtering is used throughout: rejects a single ADC
glitch without complicating the math.

WSL-side compatibility:
  - Stored values are ALREADY offset-corrected (we subtract here before
    log_value). KB2's existing legacy 2-null offset method will run on the
    null channels (sat_3:1 / sat_4:6) and find ~0 offset (because we
    already subtracted), so it applies ~0 additional correction → math
    works correctly with no WSL change.
  - V_PSU stays defaulted to 15.6 V on the WSL side. KB2's calibration
    helper checks popup.v_psu — since we don't publish it here, it falls
    back to the legacy path. Decoupled improvement for later.

Compatibility: same __init__ signature, same chain names, same
hash_logging key "IRRIGATION_VALVE_TEST", same past_actions events.
Drop-in replacement.
"""

import time

from .irrigation_logging_py3 import Hash_Logging_Object


# Settle wait after disable_all_sprinklers, before the next valve is turned on.
# Flyback diodes on the terminal blocks clamp the inductive switch-off
# transient — 3 s gives a wide margin over the actual <10 ms decay.
INTER_VALVE_SETTLE_S = 3.0

# After enabling a valve, brief wait for the coil to fully energize and
# the ACS712 reading to settle to the on-state value.
VALVE_SETTLE_S = 0.3

# Sample count per measurement. Median rejects a single ADC glitch.
PER_VALVE_SAMPLES = 5
OFFSET_SAMPLES    = 5

# Small gap between consecutive ACS712 reads — avoids sampling the same
# ADC clock cycle if measure_valve_current is just an immediate read.
SAMPLE_GAP_S = 0.05


def _median(samples):
    """Median of a list of numbers. Returns the middle element of the
    sorted list (for odd N) or the lower-middle for even N — fine for
    noise rejection."""
    s = sorted(samples)
    return s[len(s) // 2]


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

        # Sensor null offset (A) — measured in measure_offset at the start
        # of every cycle, subtracted from each per-valve reading before
        # storage.
        self.offset = 0.0

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
        cf.insert.one_step(self.measure_offset)
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
    # Offset measurement — runs ONCE at the start of each resistance check
    # ------------------------------------------------------------------
    def measure_offset(self, *args):
        """
        All valves off, wait 3 s, sample ACS712 5 times, take median.
        Sets self.offset for use by every per-valve measurement this cycle.

        Replaces the prior no-op (was a 2-second sleep + commented-out work).
        """
        self.io_control.clear_max_currents()
        self.io_control.enable_irrigation_relay()
        self.io_control.disable_all_sprinklers()

        time.sleep(INTER_VALVE_SETTLE_S)

        samples = []
        for _ in range(OFFSET_SAMPLES):
            samples.append(self.io_control.measure_valve_current())
            time.sleep(SAMPLE_GAP_S)
        self.offset = _median(samples)
        print("sensor null offset = {:.4f} A (median of {})".format(
            self.offset, OFFSET_SAMPLES))

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
    # Per-valve test — runs once per valve in the queue
    # ------------------------------------------------------------------
    def valve_setup(self, cf_handle, chainObj, parameters, event):
        if event["name"] == "INIT":
            return "CONTINUE"

        # Inter-valve settle. The prior valve was disabled at the end of
        # the last valve_measurement; flyback diodes on the terminal
        # blocks clamp the inductive switch-off, so 3 s lets the coil
        # current fully dissipate and the ACS712 settle to its true
        # off-state offset.
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

        # Brief energize settle before sampling — coil reaches steady
        # current and the ACS712 reading stabilizes.
        time.sleep(VALVE_SETTLE_S)

    def valve_measurement(self, cf_handle, chainObj, parameters, event):
        if event["name"] == "INIT":
            return "CONTINUE"

        # Take PER_VALVE_SAMPLES readings, median rejects a single ADC
        # glitch. Median (not mean) for the same noise-robustness reason
        # we use it on the offset measurement.
        samples = []
        for _ in range(PER_VALVE_SAMPLES):
            samples.append(self.io_control.measure_valve_current())
            time.sleep(SAMPLE_GAP_S)
        coil_current_raw = _median(samples)
        coil_current = coil_current_raw - self.offset

        print("coil_current limit={:.3f} raw={:.4f} offset={:.4f} corrected={:.4f}".format(
            self.irrigation_current_limit, coil_current_raw,
            self.offset, coil_current))

        # Existing over-current safety path — unchanged.
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

        # Store the OFFSET-CORRECTED current. Downstream KB2 reads this
        # as if it were the true coil current — no further offset
        # correction needed. The legacy 2-null code on the WSL side will
        # compute a near-zero offset from sat_3:1 / sat_4:6 (since those
        # are now also already-corrected) and apply that ~0 → correct R.
        self.hash_logging.log_value(logging_key, coil_current)

        # Turn this valve off. The 3-second settle for the NEXT valve
        # happens at the start of the NEXT valve_setup, so we don't add
        # latency to the last iteration of the cycle.
        self.io_control.disable_all_sprinklers()

    def log_valve_check(self, *args):
        # Diagnostic only — current_offset surfaces in past_actions so
        # we can see what offset was applied for this cycle without
        # having to read the per-valve log.
        self.handlers["IRRIGATION_PAST_ACTIONS"].push({
            "action":  "valve_resistance_done",
            "details": {
                "sensor_offset": self.offset,
            },
            "level":   "GREEN"})


if __name__ == "__main__":
    pass
