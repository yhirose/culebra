#include "tree_sitter/parser.h"

// External scanner providing one token: TRIPLE_CONTENT — a run of triple-string
// characters. The regex subset Tree-sitter compiles can't express "up to but
// not including the next `"""`", so we scan it by hand. The run stops (without
// consuming the stopper) before a `"""` delimiter, a `{` (interpolation), a `\`
// (escape), or EOF. A lone `"` or `""` not forming `"""` is ordinary content.
// No state is carried, so (de)serialization is empty.

enum TokenType { TRIPLE_CONTENT };

void *tree_sitter_culebra_external_scanner_create(void) { return 0; }
void tree_sitter_culebra_external_scanner_destroy(void *payload) { (void)payload; }
unsigned tree_sitter_culebra_external_scanner_serialize(void *payload, char *buffer) {
  (void)payload;
  (void)buffer;
  return 0;
}
void tree_sitter_culebra_external_scanner_deserialize(void *payload, const char *buffer,
                                                      unsigned length) {
  (void)payload;
  (void)buffer;
  (void)length;
}

bool tree_sitter_culebra_external_scanner_scan(void *payload, TSLexer *lexer,
                                               const bool *valid_symbols) {
  (void)payload;
  if (!valid_symbols[TRIPLE_CONTENT]) return false;

  bool consumed = false;
  lexer->mark_end(lexer);  // token end starts empty; advanced past it = lookahead
  while (!lexer->eof(lexer)) {
    int32_t c = lexer->lookahead;
    if (c == '\\' || c == '{') break;  // escape / interpolation: let the grammar take it
    if (c == '"') {
      // Count consecutive quotes (up to 3). Three is the closing delimiter and
      // must NOT be part of the content; one or two are ordinary characters.
      unsigned q = 0;
      while (lexer->lookahead == '"' && q < 3) {
        lexer->advance(lexer, false);
        q++;
      }
      if (q == 3) break;  // closing """ — beyond mark_end, so re-lexed by grammar
      consumed = true;    // 1-2 quotes are content
      lexer->mark_end(lexer);
      continue;
    }
    lexer->advance(lexer, false);
    consumed = true;
    lexer->mark_end(lexer);
  }

  if (consumed) {
    lexer->result_symbol = TRIPLE_CONTENT;
    return true;
  }
  return false;
}
