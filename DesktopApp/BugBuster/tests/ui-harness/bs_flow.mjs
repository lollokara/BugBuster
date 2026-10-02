// Connect over HTTP, open Battery Sim run #1, optional wheel zoom, screenshot.
// node bs_flow.mjs <out.png> [zoomSteps]
import { chromium } from "playwright";
const [, , out, zoom = "0"] = process.argv;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const browser = await chromium.connectOverCDP("http://127.0.0.1:9333");
const page = browser.contexts()[0].pages().find((p) => p.url().includes("localhost:1420"));
const click = (pred) => page.evaluate((src) => {
  const f = new Function("e", "return " + src);
  const el = [...document.querySelectorAll("button, a, li, [role=button]")].find((e) => f(e));
  if (el) el.click();
  return !!el;
}, pred);
if (await page.evaluate(() => !!document.querySelector(".tab-container") === false)) {
  for (let i = 0; i < 10; i++) {
    if (await click("e.textContent.includes('192.168.3.51') && e.tagName === 'BUTTON'")) break;
    if (i % 3 === 0) await click("e.textContent.trim() === 'Scan for Devices'");
    await sleep(2000);
  }
  await sleep(6000);
}
await click("e.textContent.trim() === 'Battery Sim'");
await page.waitForSelector("select.bs-pick option[value='d:1']", { state: "attached", timeout: 20000 });
await page.evaluate(() => { const s = document.querySelector("select.bs-pick"); s.value = "d:1"; s.dispatchEvent(new Event("change", { bubbles: true })); });
await page.waitForFunction(() => !document.querySelector(".bs-empty"), null, { timeout: 30000 });
await sleep(1500);
for (let k = 0; k < Number(zoom); k++) {
  await page.evaluate(() => {
    const c = document.querySelector(".bs-canvas"); const r = c.getBoundingClientRect();
    c.dispatchEvent(new WheelEvent("wheel", { clientX: r.left + r.width * 0.75, clientY: r.top + 300, deltaY: -100, bubbles: true, cancelable: true }));
  });
  await sleep(300);
}
await page.evaluate(() => {
  const c = document.querySelector(".bs-canvas"); const r = c.getBoundingClientRect();
  c.dispatchEvent(new MouseEvent("mousemove", { clientX: r.left + r.width * 0.55, clientY: r.top + 200, bubbles: true }));
});
await sleep(1200);
await page.screenshot({ path: out });
console.log("saved", out);
await browser.close().catch(() => {});
