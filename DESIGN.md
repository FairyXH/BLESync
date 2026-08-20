# BLESync Design

## Architecture

The executable has two modes:

1. Controller mode: resolves its own directory, reads the adjacent INI, requests elevation when needed, creates/updates the `BLESync` service, configures delayed auto-start and bounded recovery, starts the service, then exits.
2. Service mode: registers with SCM, reports state transitions, starts a worker, and shuts down through the service control handler.

Core modules are separated into configuration, logging, security/ACL, registry traversal, Bluetooth state detection, snapshot serialization, synchronization policy, and SCM integration.

## Registry model

A snapshot is a typed recursive tree of keys and values. It supports `REG_SZ`, `REG_EXPAND_SZ`, `REG_BINARY`, `REG_DWORD`, `REG_QWORD`, and `REG_MULTI_SZ`. Registry view is explicitly 64-bit on a 64-bit OS. Snapshot files contain a version, schema, source path, item counts, and content digest. Sensitive key payloads are only handled by the LocalSystem service and are redacted from logs.

The first implementation treats `BTHPORT\\Parameters\\Devices` and `BTHPORT\\Parameters\\Keys` as the primary state. PnP-generated `Enum` and interface trees are observed for diagnostics but are not mechanically replicated because device instance IDs, containers, interfaces, and driver state are installation-specific.

## Lifecycle and state machine

The worker transitions through:

`INITIALIZING -> WAITING_FOR_BLUETOOTH -> BLUETOOTH_DISABLED | BLUETOOTH_ENABLING | BLUETOOTH_ACTIVE -> MONITORING`

A synchronization operation temporarily enters `SYNCING` or `RESTORING`. `UNKNOWN`, `PAIRING`, and service pending states are conservative gates: no restore and no service control.

Service state is read with SCM `QueryServiceStatusEx`. Adapter/radio state is read independently using SetupAPI, Configuration Manager, and Bluetooth radio APIs. `bthserv == RUNNING` is never treated as equivalent to an enabled radio.

## User-first synchronization

The central ambiguity is whether a local/storage difference came from user intent or from a previous restore. The worker maintains:

- local snapshot hash and generation;
- storage metadata version, origin ID, timestamp, and hash;
- expected post-restore hash;
- restore generation and a bounded verification deadline;
- last stable local hash and observation time.

A stable local change observed outside the restore verification window is published as the new user state. A storage update is restored only if local Bluetooth is enabled and stable, no local change is pending, and the storage version is newer than the last applied version. This prevents a pairing or removal action from being immediately overwritten.

If both systems publish against the same base version, the service records a conflict and applies the configured conservative winner: the most recently observed stable snapshot only when the other writer has not advanced the version; otherwise it preserves the current local state and waits for the next stable convergence event. Secrets are not included in conflict logs.

## Restore safety

No destructive whole-hive import exists. Restore is key/value-level and limited to the approved BTHPORT roots. Missing values and subkeys are deleted only when the operation is explicitly classified as an approved mirror update; generated PnP trees are never deleted. Restore is skipped for disabled/unknown/pending states and on invalid or partially written storage.

The service never uses `reg.exe`, `net stop`, or periodic `sc stop/start`. A Bluetooth service restart is not part of the normal algorithm. If a future diagnostic feature proves a refresh is required, it must add a separate guarded operation with explicit state, pairing-activity checks, and an audit record.

## Atomic persistence and integrity

Writers serialize through a named mutex. The payload is written to `*.tmp`, flushed, optionally backed up, and replaced. Metadata is committed after payload validation. On startup, invalid or mismatched data is rejected and the current registry is preserved. A previous backup remains available for recovery.

## Service security

The service runs as LocalSystem because `BTHPORT\\Parameters\\Keys` is protected. The program does not modify the registry's existing permissions. Storage ACLs grant access only to LocalSystem and administrators. Configuration is not treated as a secret; key payloads are.

## Verification boundaries

A build can verify source-level behavior, service registration, state detection, snapshot round trips, atomic writes, ACL application, and no-service-restart policy. It cannot prove cross-install Bluetooth pairing compatibility without two controlled Windows installations and a known adapter. The lab protocol is documented in `docs\\BluetoothRegistryAnalysis.md`.
