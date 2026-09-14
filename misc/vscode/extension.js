// VSCode extension for Culebra: a client for `culebra lsp`, which supplies the
// diagnostics, hover, completion, formatting, definitions, references,
// highlights, the outline and rename. The debugger is registered in package.json
// (it launches `culebra dap`), and highlighting is the TextMate grammar.
//
// The client speaks the protocol's base framing — a Content-Length header, then
// a JSON-RPC body — over the server's stdio itself, and covers only what the
// server offers. That keeps the extension to this one file with no npm
// packages, which is what lets `culebra init` carry it inside the binary.

const vscode = require('vscode');
const { spawn } = require('child_process');

// Path to the `culebra` binary. `culebra init` and build-vsix.sh bake in an
// absolute path when they know one; otherwise it resolves on the runtime PATH.
const CULEBRA = 'culebra';

// A server that exits this many times is left stopped instead of respawned.
const MAX_RESTARTS = 3;

const SEVERITIES = [
  undefined,
  vscode.DiagnosticSeverity.Error,
  vscode.DiagnosticSeverity.Warning,
  vscode.DiagnosticSeverity.Information,
  vscode.DiagnosticSeverity.Hint,
];

function toRange(r) {
  return new vscode.Range(r.start.line, r.start.character, r.end.line, r.end.character);
}

function toLocation(l) {
  return new vscode.Location(vscode.Uri.parse(l.uri), toRange(l.range));
}

function toSymbol(s) {
  // LSP counts SymbolKind from 1, VSCode from 0.
  const out = new vscode.DocumentSymbol(s.name, '', s.kind - 1,
    toRange(s.range), toRange(s.selectionRange));
  out.children = (s.children || []).map(toSymbol);
  return out;
}

function at(doc, pos, extra = {}) {
  return {
    textDocument: { uri: doc.uri.toString() },
    position: { line: pos.line, character: pos.character },
    ...extra,
  };
}

// Only buffers backed by a file or not yet saved: a `git:` or diff view of the
// same file would publish its diagnostics a second time.
function isCulebra(doc) {
  return doc.languageId === 'culebra' &&
    (doc.uri.scheme === 'file' || doc.uri.scheme === 'untitled');
}

class Client {
  constructor(channel, diagnostics) {
    this.channel = channel;
    this.diagnostics = diagnostics;
    this.proc = null;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.initialized = false;
    this.stopping = false;
    this.restarts = 0;
  }

  start() {
    this.buffer = Buffer.alloc(0);
    this.initialized = false;
    const proc = spawn(CULEBRA, ['lsp'], { stdio: ['pipe', 'pipe', 'pipe'] });
    this.proc = proc;
    proc.on('error', (err) => {
      if (err.code === 'ENOENT') {
        this.stopping = true;  // nothing to restart
        vscode.window.showWarningMessage(
          `Culebra: '${CULEBRA}' not found — install it on your PATH.`);
      } else {
        this.channel.appendLine(`culebra lsp: ${err.message}`);
      }
    });
    proc.stderr.on('data', (d) => this.channel.append(d.toString()));
    proc.stdout.on('data', (d) => this.receive(d));
    proc.on('close', (code) => this.exited(proc, code));

    this.request('initialize', { processId: process.pid, rootUri: null, capabilities: {} })
      .then(() => {
        this.notify('initialized', {});
        this.initialized = true;
        for (const doc of vscode.workspace.textDocuments) this.open(doc);
      })
      .catch(() => {});
  }

  exited(proc, code) {
    if (proc !== this.proc) return;
    for (const { reject } of this.pending.values()) reject(new Error('culebra lsp exited'));
    this.pending.clear();
    this.proc = null;
    this.initialized = false;
    if (this.stopping) return;
    this.diagnostics.clear();
    if (++this.restarts <= MAX_RESTARTS) {
      this.channel.appendLine(`culebra lsp exited (${code}); restarting`);
      this.start();
    } else {
      vscode.window.showWarningMessage(
        'Culebra: the language server keeps exiting; see the Culebra output channel.');
    }
  }

  // ---- framing --------------------------------------------------------------

  receive(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      const end = this.buffer.indexOf('\r\n\r\n');
      if (end < 0) return;
      const header = this.buffer.subarray(0, end).toString('ascii');
      const m = /Content-Length: *(\d+)/i.exec(header);
      const length = m ? parseInt(m[1], 10) : 0;
      if (this.buffer.length < end + 4 + length) return;
      const body = this.buffer.subarray(end + 4, end + 4 + length).toString('utf8');
      this.buffer = this.buffer.subarray(end + 4 + length);
      let msg;
      try {
        msg = JSON.parse(body);
      } catch {
        continue;
      }
      this.dispatch(msg);
    }
  }

  send(msg) {
    if (!this.proc) return;
    const body = Buffer.from(JSON.stringify(msg), 'utf8');
    this.proc.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
    this.proc.stdin.write(body);
  }

  request(method, params) {
    return new Promise((resolve, reject) => {
      if (!this.proc) {
        reject(new Error('culebra lsp is not running'));
        return;
      }
      const id = this.nextId++;
      this.pending.set(id, { resolve, reject });
      this.send({ jsonrpc: '2.0', id, method, params });
    });
  }

  notify(method, params) {
    this.send({ jsonrpc: '2.0', method, params });
  }

  dispatch(msg) {
    if (msg.method === undefined) {
      const p = this.pending.get(msg.id);
      if (!p) return;
      this.pending.delete(msg.id);
      if (msg.error) p.reject(new Error(msg.error.message));
      else p.resolve(msg.result);
    } else if (msg.method === 'textDocument/publishDiagnostics') {
      this.publish(msg.params);
    } else if (msg.method === 'window/logMessage') {
      this.channel.appendLine(msg.params.message);
    }
  }

  // ---- documents ------------------------------------------------------------

  open(doc) {
    if (!this.initialized || !isCulebra(doc)) return;
    this.notify('textDocument/didOpen', {
      textDocument: {
        uri: doc.uri.toString(),
        languageId: 'culebra',
        version: doc.version,
        text: doc.getText(),
      },
    });
  }

  change(e) {
    const doc = e.document;
    if (!this.initialized || !isCulebra(doc) || e.contentChanges.length === 0) return;
    this.notify('textDocument/didChange', {
      textDocument: { uri: doc.uri.toString(), version: doc.version },
      contentChanges: [{ text: doc.getText() }],
    });
  }

  save(doc) {
    if (!this.initialized || !isCulebra(doc)) return;
    this.notify('textDocument/didSave', { textDocument: { uri: doc.uri.toString() } });
  }

  close(doc) {
    if (!isCulebra(doc)) return;
    this.diagnostics.delete(doc.uri);
    if (!this.initialized) return;
    this.notify('textDocument/didClose', { textDocument: { uri: doc.uri.toString() } });
  }

  publish({ uri, diagnostics }) {
    this.diagnostics.set(vscode.Uri.parse(uri), diagnostics.map((d) => {
      const out = new vscode.Diagnostic(toRange(d.range), d.message,
        SEVERITIES[d.severity] ?? vscode.DiagnosticSeverity.Error);
      out.source = d.source;
      out.code = d.code;
      return out;
    }));
  }

  // ---- requests -------------------------------------------------------------

  async hover(doc, pos) {
    const r = await this.request('textDocument/hover', {
      textDocument: { uri: doc.uri.toString() },
      position: { line: pos.line, character: pos.character },
    });
    if (!r) return null;
    return new vscode.Hover(new vscode.MarkdownString(r.contents.value),
      r.range ? toRange(r.range) : undefined);
  }

  async definition(doc, pos) {
    const r = await this.request('textDocument/definition', at(doc, pos));
    return r ? r.map(toLocation) : null;
  }

  async references(doc, pos, context) {
    const r = await this.request('textDocument/references',
      at(doc, pos, { context: { includeDeclaration: context.includeDeclaration } }));
    return (r || []).map(toLocation);
  }

  async highlights(doc, pos) {
    const r = await this.request('textDocument/documentHighlight', at(doc, pos));
    return (r || []).map((h) => new vscode.DocumentHighlight(toRange(h.range),
      h.kind === 3 ? vscode.DocumentHighlightKind.Write : vscode.DocumentHighlightKind.Read));
  }

  async symbols(doc) {
    const r = await this.request('textDocument/documentSymbol',
      { textDocument: { uri: doc.uri.toString() } });
    return (r || []).map(toSymbol);
  }

  async completion(doc, pos) {
    const r = await this.request('textDocument/completion', at(doc, pos));
    return ((r && r.items) || []).map((i) => {
      // LSP counts CompletionItemKind from 1, VSCode from 0.
      const item = new vscode.CompletionItem(i.label, i.kind - 1);
      item.detail = i.detail;
      item.sortText = i.sortText;
      return item;
    });
  }

  // A refusal comes back as an error, whose message VSCode shows as it stands.
  async prepareRename(doc, pos) {
    const r = await this.request('textDocument/prepareRename', at(doc, pos));
    if (!r) throw new Error('There is no name here to rename.');
    return { range: toRange(r.range), placeholder: r.placeholder };
  }

  async rename(doc, pos, newName) {
    const r = await this.request('textDocument/rename', at(doc, pos, { newName }));
    const edit = new vscode.WorkspaceEdit();
    for (const [uri, edits] of Object.entries((r && r.changes) || {})) {
      for (const e of edits) edit.replace(vscode.Uri.parse(uri), toRange(e.range), e.newText);
    }
    return edit;
  }

  // `culebra fmt` is a whole-file formatter, so only document formatting exists.
  async format(doc) {
    const edits = await this.request('textDocument/formatting', {
      textDocument: { uri: doc.uri.toString() },
      options: { tabSize: 2, insertSpaces: true },
    });
    return (edits || []).map((e) => vscode.TextEdit.replace(toRange(e.range), e.newText));
  }

  async stop() {
    this.stopping = true;
    const proc = this.proc;
    if (!proc) return;
    const timeout = new Promise((resolve) => setTimeout(resolve, 1000));
    await Promise.race([this.request('shutdown', null).catch(() => {}), timeout]);
    this.notify('exit', null);
    setTimeout(() => proc.kill(), 1000);
  }
}

let client = null;

function activate(context) {
  const channel = vscode.window.createOutputChannel('Culebra');
  const diagnostics = vscode.languages.createDiagnosticCollection('culebra');
  client = new Client(channel, diagnostics);
  client.start();

  const selector = { language: 'culebra' };
  context.subscriptions.push(
    channel,
    diagnostics,
    vscode.workspace.onDidOpenTextDocument((d) => client.open(d)),
    vscode.workspace.onDidChangeTextDocument((e) => client.change(e)),
    vscode.workspace.onDidSaveTextDocument((d) => client.save(d)),
    vscode.workspace.onDidCloseTextDocument((d) => client.close(d)),
    vscode.languages.registerHoverProvider(selector, {
      provideHover: (doc, pos) => client.hover(doc, pos).catch(() => null),
    }),
    vscode.languages.registerDocumentFormattingEditProvider(selector, {
      provideDocumentFormattingEdits: (doc) => client.format(doc).catch(() => []),
    }),
    vscode.languages.registerDefinitionProvider(selector, {
      provideDefinition: (doc, pos) => client.definition(doc, pos).catch(() => null),
    }),
    vscode.languages.registerReferenceProvider(selector, {
      provideReferences: (doc, pos, context) =>
        client.references(doc, pos, context).catch(() => []),
    }),
    vscode.languages.registerDocumentHighlightProvider(selector, {
      provideDocumentHighlights: (doc, pos) => client.highlights(doc, pos).catch(() => []),
    }),
    vscode.languages.registerDocumentSymbolProvider(selector, {
      provideDocumentSymbols: (doc) => client.symbols(doc).catch(() => []),
    }),
    vscode.languages.registerRenameProvider(selector, {
      prepareRename: (doc, pos) => client.prepareRename(doc, pos),
      provideRenameEdits: (doc, pos, newName) => client.rename(doc, pos, newName),
    }),
    vscode.languages.registerCompletionItemProvider(selector, {
      provideCompletionItems: (doc, pos) => client.completion(doc, pos).catch(() => []),
    }, '.'),
  );
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
