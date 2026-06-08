#!/usr/bin/env python3
"""Render the BugBuster GLB into a README-friendly animated GIF.

Requires Pillow and a local Chromium/Chrome binary. Set CHROME_BIN if Chrome is
not in the default macOS location.
"""

from __future__ import annotations

import contextlib
import functools
import http.server
import json
import os
import pathlib
import socketserver
import subprocess
import tempfile
import threading
from PIL import Image


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODEL = "DesktopApp/BugBuster/public/models/bugbuster.glb"
OUT = ROOT / "Docs/Images/bugbuster_pcb_3d.gif"
WIDTH = 960
HEIGHT = 560
FRAMES = 96


HTML = """<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <style>
    html, body { margin: 0; width: 100%; height: 100%; overflow: hidden; background: #050914; }
    canvas { display: block; width: 100vw; height: 100vh; }
  </style>
  <script type="importmap">
    {"imports":{"three":"/DesktopApp/BugBuster/public/vendor/three.module.min.js","three/addons/GLTFLoader.js":"/DesktopApp/BugBuster/public/vendor/GLTFLoader.js"}}
  </script>
</head>
<body>
<canvas id="c"></canvas>
<script type="module">
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/GLTFLoader.js';

const params = new URLSearchParams(location.search);
const frame = Number(params.get('frame') || 0);
const total = Number(params.get('total') || 36);
const canvas = document.getElementById('c');
const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(32, innerWidth / innerHeight, 0.1, 1000);
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: false });
renderer.setPixelRatio(1);
renderer.setSize(innerWidth, innerHeight);
renderer.setClearColor(0x0d1117, 1);
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.0;
renderer.outputColorSpace = THREE.SRGBColorSpace;
scene.background = new THREE.Color(0x0d1117);

scene.add(new THREE.HemisphereLight(0xffffff, 0x0f172a, 0.22));
const key = new THREE.DirectionalLight(0xffffff, 0.85);
key.position.set(150, 220, 140);
scene.add(key);
const cyan = new THREE.DirectionalLight(0x00e5ff, 1.8);
cyan.position.set(-15, 5, 10);
scene.add(cyan);
const blue = new THREE.DirectionalLight(0x3b82f6, 1.25);
blue.position.set(15, 4, 10);
scene.add(blue);
const rim = new THREE.DirectionalLight(0xffffff, 0.7);
rim.position.set(0, 150, -200);
scene.add(rim);

const loader = new GLTFLoader();
loader.load('/""" + MODEL + """', (gltf) => {
  const model = gltf.scene;
  model.traverse((child) => {
    if (!child.isMesh || !child.material) return;
    const mats = Array.isArray(child.material) ? child.material : [child.material];
    mats.forEach((mat) => {
      if (!mat.color) return;
      const hex = mat.color.getHexString().toLowerCase();
      const sr = parseInt(hex.slice(0, 2), 16) / 255;
      const sg = parseInt(hex.slice(2, 4), 16) / 255;
      const sb = parseInt(hex.slice(4, 6), 16) / 255;
      if (sg > sr * 1.15 && sg > sb * 1.15 && sg > 0.12) {
        child.geometry.computeBoundingBox();
        const size = new THREE.Vector3();
        child.geometry.boundingBox.getSize(size);
        if (size.y < 0.0035) {
          mat.color.setHex(0x115232);
          mat.roughness = 0.95;
          mat.metalness = 0.0;
        } else {
          mat.color.setHex(0x181818);
          mat.roughness = 0.70;
          mat.metalness = 0.05;
        }
      } else if (sr > 0.45 && sg > 0.35 && sb < 0.65 && sr > sb * 1.3 && sg > sb * 1.1) {
        mat.color.setHex(0xe5a93b);
        mat.roughness = 0.15;
        mat.metalness = 0.4;
      } else if (Math.abs(sr - sg) < 0.08 && Math.abs(sg - sb) < 0.08 && sr > 0.35) {
        mat.roughness = 0.20;
        mat.metalness = 0.30;
      } else if (sr < 0.32 && sg < 0.32 && sb < 0.32) {
        mat.roughness = 0.70;
        mat.metalness = 0.05;
        mat.color.multiplyScalar(0.6);
      }
      mat.needsUpdate = true;
    });
  });

  const box = new THREE.Box3().setFromObject(model);
  const center = new THREE.Vector3();
  const size = new THREE.Vector3();
  box.getCenter(center);
  box.getSize(size);
  model.position.sub(center);

  const group = new THREE.Group();
  group.add(model);
  scene.add(group);

  const maxDim = Math.max(size.x, size.y, size.z);
  const fov = camera.fov * Math.PI / 180;
  const cameraZ = Math.abs(maxDim / 2 / Math.tan(fov / 2)) / 0.72;
  const tilt = 24 * Math.PI / 180;
  camera.position.set(0, cameraZ * Math.sin(tilt), cameraZ * Math.cos(tilt));
  camera.lookAt(0, 0, 0);

  const phase = frame / total;
  group.rotation.order = 'YXZ';
  group.rotation.y = phase * Math.PI * 2;
  group.rotation.x = Math.sin(phase * Math.PI * 2) * 0.08;
  group.rotation.z = Math.cos(phase * Math.PI * 2) * 0.04;

  const ledColor = frame > total * 0.45 ? 0x00ff44 : 0xffaa00;
  const led = new THREE.PointLight(ledColor, 0.08, 0.04, 2);
  led.position.set(-0.021, -0.016, 0.038);
  model.add(led);

  renderer.render(scene, camera);
  window.__ready = true;
}, undefined, (err) => {
  document.body.dataset.error = String(err?.message || err);
  window.__ready = true;
});
</script>
</body>
</html>
"""


def chrome_bin() -> str:
    env = os.environ.get("CHROME_BIN")
    if env:
        return env
    mac = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if pathlib.Path(mac).exists():
        return mac
    return "chrome"


@contextlib.contextmanager
def server():
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=ROOT)
    with socketserver.TCPServer(("127.0.0.1", 0), handler) as httpd:
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        try:
            yield httpd.server_address[1]
        finally:
            httpd.shutdown()
            thread.join()


def main() -> None:
    html_path = ROOT / "Docs/Images/render_pcb_animation.html"
    with tempfile.TemporaryDirectory() as tmpdir, server() as port:
        tmp = pathlib.Path(tmpdir)
        html_path.write_text(HTML, encoding="utf-8")
        try:
            playwright_dir = ROOT / "DesktopApp/BugBuster/tests/e2e/node_modules/playwright"
            capture_script = tmp / "capture.js"
            capture_script.write_text(
                """
const { chromium } = require(process.env.PLAYWRIGHT_MODULE);

(async () => {
  const cfg = JSON.parse(process.env.CAPTURE_CONFIG);
  const browser = await chromium.launch({
    executablePath: cfg.chrome,
    args: ['--disable-gpu'],
  });
  const page = await browser.newPage({
    viewport: { width: cfg.width, height: cfg.height },
    deviceScaleFactor: 1,
  });
  page.on('pageerror', (err) => console.error('[pageerror]', err.message));
  for (let i = 0; i < cfg.frames; i++) {
    const url = `${cfg.baseUrl}?frame=${i}&total=${cfg.frames}`;
    await page.goto(url, { waitUntil: 'networkidle' });
    await page.waitForFunction(() => window.__ready === true, null, { timeout: 20000 });
    await page.screenshot({ path: `${cfg.tmp}/frame-${String(i).padStart(3, '0')}.png` });
  }
  await browser.close();
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
""",
                encoding="utf-8",
            )
            env = os.environ.copy()
            env["PLAYWRIGHT_MODULE"] = str(playwright_dir)
            env["CAPTURE_CONFIG"] = json.dumps(
                {
                    "baseUrl": f"http://127.0.0.1:{port}/Docs/Images/render_pcb_animation.html",
                    "chrome": chrome_bin(),
                    "frames": FRAMES,
                    "height": HEIGHT,
                    "tmp": str(tmp),
                    "width": WIDTH,
                }
            )
            subprocess.run(["node", str(capture_script)], check=True, env=env)

            frames = [
                Image.open(tmp / f"frame-{i:03d}.png").convert("P", palette=Image.Palette.ADAPTIVE)
                for i in range(FRAMES)
            ]

            frames[0].save(
                OUT,
                save_all=True,
                append_images=frames[1:],
                duration=80,
                loop=0,
                optimize=True,
            )
        finally:
            html_path.unlink(missing_ok=True)
    print(f"Wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
