-- dongle_registry.lua — motioncore dongle instance registry
--
-- Roster of physical dongles known to the Linux side: one row per commissioned
-- dongle, keyed by chip_uid (the immutable 32-hex factory unique id).
--
-- MACHINE-MAINTAINED by commission.lua — --set adds/updates a row, --clear
-- removes one. Do not hand-edit; re-run the tool instead.
--
-- Row fields:  class_id (u32)   instance_id (u32)   role (string)

return {
  ["0B261633333032583333B2F72F30574B"] = { class_id = 0x281A0BA4, instance_id = 1, role = "bench" },
  ["2667118A3432585020312E351E1501FF"] = { class_id = 0x5E588873, instance_id = 2, role = "bench" },
  ["508880F73432585020312E35271601FF"] = { class_id = 0x5E588873, instance_id = 1, role = "bench" },
}
