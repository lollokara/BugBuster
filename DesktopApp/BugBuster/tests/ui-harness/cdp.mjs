// Drive the running Tauri WebView2 over CDP: node cdp.mjs <action> [arg]
// actions: shot <file> | eval <js> | click <text> | size
import { chromium } from "playwright";

const [, , action, arg] = process.argv;
const browser = await chromium.connectOverCDP("http://127.0.0.1:9333");
const ctx = browser.contexts()[0];
const page = ctx.pages().find((p) => p.url().includes("localhost:1420")) ?? ctx.pages()[0];
try {
  if (action === "shot") {
    await page.screenshot({ path: arg });
    console.log("saved", arg, page.url());
  } else if (action === "eval") {
    const r = await page.evaluate(arg);
    console.log(typeof r === "string" ? r : JSON.stringify(r, null, 1));
  } else if (action === "click") {
    const r = await page.evaluate((t) => {
      const els = [...document.querySelectorAll("button, a, [role=button], .nav-item, li, span")];
      const el = els.find((e) => e.textContent.trim() === t) ?? els.find((e) => e.textContent.trim().startsWith(t));
      if (!el) return "not found";
      el.click();
      return "clicked " + el.tagName + "." + el.className;
    }, arg);
    console.log(r);
  } else if (action === "size") {
    console.log(JSON.stringify(await page.evaluate(() => [innerWidth, innerHeight, devicePixelRatio])));
  }
} finally {
  await browser.close().catch(() => {});
}
