// Zed extension entry point for Culebra. Registers the `culebra` debug adapter,
// which Zed can't do declaratively — it needs this thin WASM shim that tells Zed
// how to launch the adapter. The adapter itself is `culebra dap` (a stdio DAP
// server shipped in the culebra binary); all the debugging logic lives there.
//
// The grammar + language config (syntax highlighting) are declared in
// extension.toml / languages/ and need no code here.

use zed_extension_api::{
    self as zed,
    serde_json::{self, json, Value},
    DebugAdapterBinary, DebugConfig, DebugRequest, DebugScenario, DebugTaskDefinition, Result,
    StartDebuggingRequestArguments, StartDebuggingRequestArgumentsRequest, Worktree,
};

struct CulebraExtension;

impl zed::Extension for CulebraExtension {
    fn new() -> Self {
        CulebraExtension
    }

    // The culebra debugger only launches a program; it never attaches.
    fn dap_request_kind(
        &mut self,
        _adapter_name: String,
        config: Value,
    ) -> Result<StartDebuggingRequestArgumentsRequest> {
        match config.get("request").and_then(|v| v.as_str()) {
            Some("attach") => {
                Err("the culebra debugger supports only `launch`, not `attach`".into())
            }
            _ => Ok(StartDebuggingRequestArgumentsRequest::Launch),
        }
    }

    // Turn a generic debug config (e.g. from the UI) into a launch scenario.
    fn dap_config_to_scenario(&mut self, config: DebugConfig) -> Result<DebugScenario> {
        let launch = match &config.request {
            DebugRequest::Launch(launch) => launch,
            DebugRequest::Attach(_) => {
                return Err("the culebra debugger supports only `launch`, not `attach`".into())
            }
        };
        let obj = json!({
            "request": "launch",
            "program": launch.program,
            "cwd": launch.cwd,
            "args": launch.args,
            "stopOnEntry": config.stop_on_entry.unwrap_or(false),
        });
        Ok(DebugScenario {
            adapter: config.adapter,
            label: config.label,
            build: None,
            config: obj.to_string(),
            tcp_connection: None,
        })
    }

    // Tell Zed how to start the adapter: `culebra dap` over stdio, with the
    // scenario's config forwarded as the launch request arguments.
    fn get_dap_binary(
        &mut self,
        adapter_name: String,
        config: DebugTaskDefinition,
        user_provided_debug_adapter_path: Option<String>,
        worktree: &Worktree,
    ) -> Result<DebugAdapterBinary> {
        // Prefer an explicit path, then `culebra` on the worktree's PATH, else
        // hope it resolves at launch.
        let command = user_provided_debug_adapter_path
            .or_else(|| worktree.which("culebra"))
            .unwrap_or_else(|| "culebra".to_string());

        let mut configuration: Value = serde_json::from_str(&config.config)
            .map_err(|e| format!("invalid debug configuration JSON: {e}"))?;
        if let Some(obj) = configuration.as_object_mut() {
            obj.entry("cwd")
                .or_insert_with(|| worktree.root_path().into());
        }
        let request = self.dap_request_kind(adapter_name, configuration.clone())?;

        Ok(DebugAdapterBinary {
            command: Some(command),
            arguments: vec!["dap".into()],
            connection: None, // stdio, not TCP
            cwd: Some(worktree.root_path()),
            envs: vec![],
            request_args: StartDebuggingRequestArguments {
                request,
                configuration: configuration.to_string(),
            },
        })
    }
}

zed::register_extension!(CulebraExtension);
