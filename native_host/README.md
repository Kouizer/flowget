# native_host/

Native Messaging host that connects the browser extension to the running FlowGet instance.

Arrives in **M4**:

- Reads/writes 4-byte native-endian length-prefixed UTF-8 JSON on stdin/stdout (opened in **binary**
  mode on Windows; text mode corrupts the framing).
- Starts the app with `--enqueue <url>` if no instance is running, otherwise forwards to the existing
  one via the single-instance channel.
- `install` / `uninstall` subcommands write the host manifest and the HKCU registry keys for Chrome,
  Edge, and Firefox:
  `Software\Google\Chrome\NativeMessagingHosts\com.flowget.host`,
  `Software\Microsoft\Edge\NativeMessagingHosts\com.flowget.host`,
  `Software\Mozilla\NativeMessagingHosts\com.flowget.host`.
- Rejects messages from unknown origins and `file://` targets (see ADR-0002 and the security notes).
