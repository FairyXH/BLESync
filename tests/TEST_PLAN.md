# BLESync Test Plan

Automated checks are intentionally limited to behavior that can be safely exercised on the current host without modifying Bluetooth pairing state:

- MinGW build and executable existence.
- Adjacent INI discovery and required settings.
- Unknown CLI option returns failure.
- `--capture` can be run only in an elevated lab because it reads BTHPORT state.
- Service install/uninstall must be run in an isolated elevated VM or a dedicated test machine; do not run it on a production host without approval.

Manual lab tests required for acceptance:

1. Install and repeat install; inspect one `BLESync` service.
2. Bluetooth OFF: run service and confirm no `StartService`/`ControlService` calls against `bthserv` in Procmon/ETW.
3. Toggle ON/OFF and pair/remove one device while collecting snapshots.
4. Validate that stable local additions and removals are published.
5. Validate that malformed/truncated snapshots are rejected.
6. Use a second equivalent Windows installation with the same storage path and verify compatibility of the specific device/key schema.
