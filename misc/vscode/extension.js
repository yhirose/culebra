// VSCode formatting provider for Culebra. Runs `culebra fmt -` on the whole
// document and applies its output, but only when the command exits cleanly: a
// parse or safety error produces no stdout, and we leave the document
// untouched instead of replacing it with empty text. `culebra fmt` is a
// whole-file formatter, so only document (not range) formatting is registered.
// This enables "Format Document" and editor.formatOnSave for .cul files.

const vscode = require('vscode');
const { execFile } = require('child_process');

// Path to the `culebra` binary. build-vsix.sh bakes in an absolute path when
// one is on $PATH at package time; otherwise it resolves on the runtime PATH.
const CULEBRA = 'culebra';

function activate(context) {
  const channel = vscode.window.createOutputChannel('Culebra');
  context.subscriptions.push(channel);
  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider('culebra', {
      provideDocumentFormattingEdits(document) {
        const text = document.getText();
        return new Promise((resolve) => {
          const child = execFile(
            CULEBRA, ['fmt', '-'],
            { maxBuffer: 64 * 1024 * 1024 },
            (err, stdout, stderr) => {
              if (err) {
                if (err.code === 'ENOENT') {
                  vscode.window.showWarningMessage(
                    `Culebra: '${CULEBRA}' not found — install it on your PATH.`);
                } else if (stderr) {
                  channel.appendLine(stderr.trim());
                }
                resolve([]);  // parse / safety error -> leave the document as is
                return;
              }
              const all = new vscode.Range(
                document.positionAt(0), document.positionAt(text.length));
              resolve([vscode.TextEdit.replace(all, stdout)]);
            });
          child.stdin.end(text);
        });
      },
    }));
}

function deactivate() {}

module.exports = { activate, deactivate };
