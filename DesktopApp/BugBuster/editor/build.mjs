// Bundles editor/src/entry.js to public/scripts-editor.js (IIFE, offline) and keeps
// public/firmware-api.json identical to the canonical catalogue.
//   node build.mjs                    bundle + verify catalogue copy
//   node build.mjs --sync-catalogue   copy canonical catalogue into public/
//   node build.mjs --check            verify bundle is current and catalogue copy matches
import { build } from "esbuild";
import { copyFileSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const app = resolve(here, "..");
const canonical = resolve(app, "../../python/firmware_modules/stubs/firmware_api.json");
const copy = resolve(app, "public/firmware-api.json");
const outfile = resolve(app, "public/scripts-editor.js");
const args = new Set(process.argv.slice(2));

function sameFile(a, b) {
  return existsSync(a) && existsSync(b) && readFileSync(a).equals(readFileSync(b));
}

function bundleOptions(write) {
  return {
    entryPoints: [resolve(here, "src/entry.js")],
    bundle: true,
    format: "iife",
    target: "es2020",
    minify: true,
    legalComments: "none",
    logLevel: "warning",
    outfile,
    write,
  };
}

if (args.has("--sync-catalogue")) copyFileSync(canonical, copy);

if (!sameFile(canonical, copy)) {
  console.error("public/firmware-api.json differs from python/firmware_modules/stubs/firmware_api.json; run: npm run sync-catalogue");
  process.exit(1);
}

const result = await build(bundleOptions(false));
const fresh = Buffer.from(result.outputFiles[0].text.replace(/[\t ]+$/gm, ""));
if (args.has("--check")) {
  if (!existsSync(outfile) || !readFileSync(outfile).equals(fresh)) {
    console.error("public/scripts-editor.js is stale; run: npm run build");
    process.exit(1);
  }
  console.log("scripts-editor.js and firmware-api.json are current");
} else {
  writeFileSync(outfile, fresh);
  console.log("built", outfile);
}
