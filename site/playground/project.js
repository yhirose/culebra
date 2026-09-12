// A project is an entry source plus the files it opens, all fetched from one
// place: the Playground's own examples/ tree for a catalog entry, or the
// directory a `.cul` lives in on GitHub for a `?src=` link. GitHub's hosts
// give a browser no directory listing, so both the listing and the files
// come through jsDelivr, which fronts any public GitHub repo with CORS on,
// a CDN cache, and a tree API at data.jsdelivr.com.
//
// No DOM and no fetch: node holds these to fixtures in
// tools/playground/project_test.mjs.

// The forms a person copies: the file page, its raw view, and the CDN URL
// itself. A branch name containing a slash cannot be told from the path in
// any of them (GitHub's own URLs share the ambiguity), so ref is one segment.
// The Raw button on a file page spells the ref as refs/heads/<branch>.
const GITHUB_FORMS = [
  /^https:\/\/github\.com\/([^/]+)\/([^/]+)\/(?:blob|raw)\/(?:refs\/(?:heads|tags)\/)?([^/]+)\/(.+)$/,
  /^https:\/\/raw\.githubusercontent\.com\/([^/]+)\/([^/]+)\/(?:refs\/(?:heads|tags)\/)?([^/]+)\/(.+)$/,
  /^https:\/\/cdn\.jsdelivr\.net\/gh\/([^/]+)\/([^/@]+)@([^/]+)\/(.+)$/,
];
const GITHUB_HOSTS = ["github.com", "raw.githubusercontent.com", "cdn.jsdelivr.net"];

export function cdnBase(owner, repo, ref) {
  return `https://cdn.jsdelivr.net/gh/${owner}/${repo}@${ref}/`;
}

export function treeUrl(owner, repo, ref) {
  return `https://data.jsdelivr.com/v1/packages/gh/${owner}/${repo}@${ref}?structure=flat`;
}

// A file on GitHub -> {owner, repo, ref, path}. Any other https URL -> {url},
// a single file with nothing beside it (a gist's raw view, say). Anything
// else throws with the form that was expected.
export function parseSourceUrl(text) {
  let url;
  try { url = new URL(text); } catch { throw new Error(`?src= is not a URL: ${text}`); }
  if (url.protocol !== "https:") throw new Error(`?src= must be https: ${text}`);
  const bare = url.origin + url.pathname;   // a ?raw=true or #L12 rides along on copied links
  for (const form of GITHUB_FORMS) {
    const m = form.exec(bare);
    // Decoded: the tree API names files as they are, and the path becomes
    // the program's own (Sys.script), which a %20 has no business in.
    if (m) return { owner: m[1], repo: m[2], ref: decodeURIComponent(m[3]), path: decodeURIComponent(m[4]) };
  }
  if (GITHUB_HOSTS.includes(url.hostname)) {
    throw new Error(`?src= wants a file, like https://github.com/<owner>/<repo>/blob/<ref>/<path>.cul: ${text}`);
  }
  return { url: bare };
}

// The files beside `path` in a jsDelivr flat tree ({files: [{name, size}]},
// names starting with "/"): everything under its directory, recursively,
// minus the entry itself. A program at the repo root claims the whole repo,
// which is what the caps are for.
const MAX_FILES = 500;
const MAX_BYTES = 20 << 20;

export function projectFiles(tree, path, { maxFiles = MAX_FILES, maxBytes = MAX_BYTES } = {}) {
  const dir = path.slice(0, path.lastIndexOf("/") + 1);
  const under = tree.files.filter((f) => f.name.startsWith("/" + dir));
  const bytes = under.reduce((n, f) => n + f.size, 0);
  if (under.length > maxFiles || bytes > maxBytes) {
    throw new Error(`${dir || "the repository root"} holds ${under.length} files (${(bytes / 1048576).toFixed(1)} MB); ` +
                    `the limit is ${maxFiles} files and ${maxBytes >> 20} MB. Put the program in its own directory.`);
  }
  return under.map((f) => f.name.slice(1)).filter((name) => name !== path).sort();
}
