# Windows Bluetooth Registry Analysis (baseline)

Date: 2026-08-20

## Scope and evidence

This document records the first live inspection of the current Windows installation. It is an implementation baseline, not a claim that every Windows build stores identical data. Any restore policy must remain conservative and version-gated.

Observed environment:

- Windows 10 Pro, build 22631, x64
- `bthserv`: Running, Manual, shared `svchost.exe -k LocalService -p`
- Bluetooth PnP adapter: `Generic Bluetooth Adapter`, status OK
- A paired device and several `BTH`/`BTHENUM` enumerator entries were present
- `BTHPORT\\Parameters\\Devices` was readable as the current elevated user
- `BTHPORT\\Parameters\\Keys` denied access to the current non-SYSTEM inspection context

## Registry areas

| Path | Initial role hypothesis | Cross-install policy | Risk |
|---|---|---|---|
| `HKLM\\SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices` | Bluetooth device records, names/metadata and pairing-related device state | Primary snapshot candidate; synchronize only normalized device subtrees | Contains device-specific state; schema may vary |
| `HKLM\\SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Keys` | Adapter/device authentication material, including link keys | Primary sensitive snapshot candidate; access only from LocalSystem; encrypted/ACL-protected storage | Credential-equivalent material; never log or expose |
| `HKLM\\SYSTEM\\CurrentControlSet\\Enum\\BTH` | PnP-enumerated Bluetooth bus/devnodes | Do not blindly replicate; hardware/instance-specific | Can invalidate PnP identity on another boot |
| `HKLM\\SYSTEM\\CurrentControlSet\\Enum\\BTHENUM` | Profile/service enumerator devnodes | Do not blindly replicate; derive from live PnP | Instance and driver generated |
| `HKLM\\SYSTEM\\CurrentControlSet\\Control\\DeviceClasses` | Device interface registration | Do not replicate; system-generated indices/interfaces | Broad system side effects |
| `HKLM\\SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters` | Stack configuration and generated state | Inspect subkeys individually; no whole-tree replacement | May include adapter-specific/runtime data |

## Required differential study before production restore

1. Capture a read-only snapshot before pairing.
2. Pair one new device, wait for stack stabilization, then capture again.
3. Remove exactly that device, wait for stabilization, then capture again.
4. Toggle the radio off/on without pairing and capture service/radio/registry changes.
5. Connect/disconnect an already paired device and capture changes.
6. During each step, record `bthserv` status, radio enumeration, SetupAPI device status, and normalized snapshots.
7. If available, use Process Monitor with a path filter for `BTHPORT`, `BTH`, and `BTHENUM` to correlate writes with the user action.

The production implementation must treat the differential results as evidence. It must not infer that every value under a listed path is portable.

## Bluetooth state layers

The service keeps these dimensions separate:

- Service state: SCM state (`STOPPED`, `START_PENDING`, `RUNNING`, etc.).
- Adapter state: SetupAPI/PnP presence and problem code.
- Radio state: `BluetoothGetRadioInfo` flags where available.
- Device state: normalized BTHPORT device/key snapshot and live PnP device list.

Unknown is a valid result. Unknown states suppress automatic service control and destructive restore.

## Current limitation

A live paired device was present, but this environment did not provide a controlled before/after pairing-removal experiment or Process Monitor trace. Therefore the initial implementation must ship with conservative defaults, dry-run/diagnostic capture, and explicit documentation that cross-Windows restoration of arbitrary `Enum` and `DeviceClasses` data is unsupported.
