import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/GLTFLoader.js';

let scene, camera, renderer, animationFrameId;
let group;
let scale = 0;
let targetX = 0;
let currentX = 0;
let isScanCompleted = false;
let isDestroyed = false;

// Cursor tracking and gyroscopic tilt variables
let mouseX = 0;
let mouseY = 0;
let targetMouseX = 0;
let targetMouseY = 0;
let tiltX = 0;
let tiltZ = 0;
let cursorLight = null;
let mouseMoveListener = null;

// LED animation variables
let bootStartTime = 0;
let ledMats = [];
let ledLights = [];


function extractLEDs(gltfScene, ledPositions) {
    let mesh2 = null;
    gltfScene.traverse(child => {
        if (child.isMesh && child.name === "mesh_2") {
            mesh2 = child;
        }
    });
    
    if (!mesh2) {
        console.warn("[splash.js] mesh_2 not found");
        return [];
    }
    
    const geometry = mesh2.geometry;
    const positionAttr = geometry.attributes.position;
    const indexAttr = geometry.index;
    
    if (!indexAttr) {
        console.warn("[splash.js] mesh_2 is not indexed");
        return [];
    }
    
    const ledIndices = [[], [], []];
    const remainingIndices = [];
    const tempV = new THREE.Vector3();
    const faceCount = indexAttr.count / 3;
    
    for (let f = 0; f < faceCount; f++) {
        const a = indexAttr.getX(f * 3);
        const b = indexAttr.getY(f * 3);
        const c = indexAttr.getZ(f * 3);
        
        // Use face center coordinates to ensure we capture the whole 3D package geometry
        tempV.fromBufferAttribute(positionAttr, a);
        const ax = tempV.x, ay = tempV.y, az = tempV.z;
        tempV.fromBufferAttribute(positionAttr, b);
        const bx = tempV.x, by = tempV.y, bz = tempV.z;
        tempV.fromBufferAttribute(positionAttr, c);
        const cx = tempV.x, cy = tempV.y, cz = tempV.z;
        
        const avgX = (ax + bx + cx) / 3;
        const avgY = (ay + by + cy) / 3;
        const avgZ = (az + bz + cz) / 3;
        
        let ledIdx = -1;
        for (let i = 0; i < ledPositions.length; i++) {
            const led = ledPositions[i];
            const dx = avgX - led.x;
            const dz = avgZ - led.z;
            const distXZ = Math.sqrt(dx * dx + dz * dz);
            
            if (distXZ < 0.0016 && Math.abs(avgY - led.y) < 0.0015) {
                ledIdx = i;
                break;
            }
        }
        
        if (ledIdx !== -1) {
            ledIndices[ledIdx].push(a, b, c);
        } else {
            remainingIndices.push(a, b, c);
        }
    }
    
    console.log(`[splash.js] Split mesh_2: LED1=${ledIndices[0].length/3}, LED2=${ledIndices[1].length/3}, LED3=${ledIndices[2].length/3}, Remaining=${remainingIndices.length/3}`);
    
    // 1. Update original mesh_2 to hide the LED bodies (prevent duplicate rendering)
    const remainingGeom = geometry.clone();
    remainingGeom.setIndex(remainingIndices);
    mesh2.geometry = remainingGeom;
    
    // 2. Create 3 new separate meshes for each LED body with its own material
    const mats = [];
    for (let i = 0; i < 3; i++) {
        if (ledIndices[i].length === 0) {
            console.warn(`[splash.js] LED ${i+1} has 0 indices`);
            mats.push(new THREE.MeshStandardMaterial());
            continue;
        }
        
        const ledGeom = geometry.clone();
        ledGeom.setIndex(ledIndices[i]);
        
        const mat = new THREE.MeshStandardMaterial({
            color: 0xdddddd,
            emissive: 0xffaa00,
            emissiveIntensity: 0.0,
            roughness: 0.15,
            metalness: 0.8
        });
        mats.push(mat);
        
        const ledMesh = new THREE.Mesh(ledGeom, mat);
        mesh2.parent.add(ledMesh);
    }
    
    return mats;
}

window.initSplash = function() {
    console.log('[splash.js] initSplash triggered!');
    const canvas = document.getElementById('board-canvas');
    if (!canvas) {
        console.warn('initSplash: #board-canvas element not found');
        return;
    }
    
    // If we already have a running instance, destroy it first
    if (renderer || animationFrameId) {
        window.destroySplash();
    }
    
    isDestroyed = false;
    scale = 0;
    targetX = 0;
    currentX = 0;
    isScanCompleted = false;
    
    // Create WebGLRenderer with alpha for transparency over the particle background
    renderer = new THREE.WebGLRenderer({
        canvas: canvas,
        antialias: true,
        alpha: true
    });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.setSize(window.innerWidth, window.innerHeight);
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 1.0;
    if (THREE.SRGBColorSpace !== undefined) {
        renderer.outputColorSpace = THREE.SRGBColorSpace;
    } else if (THREE.sRGBEncoding !== undefined) {
        renderer.outputEncoding = THREE.sRGBEncoding;
    }
    
    // Create Scene
    scene = new THREE.Scene();
    
    // Create Camera
    camera = new THREE.PerspectiveCamera(35, window.innerWidth / window.innerHeight, 0.1, 10000);
    camera.position.set(0, 4, 12);
    
    // Add camera to scene so its children are rendered
    scene.add(camera);
    
    // Create cursor tracking point light (cool white)
    cursorLight = new THREE.PointLight(0xf1f5f9, 1.2, 0, 2);
    cursorLight.position.set(0, 0, -0.05); // offset slightly in front of camera lens
    camera.add(cursorLight);
    
    // Set up mouse listener & dynamic simulation variables
    mouseX = 0;
    mouseY = 0;
    targetMouseX = 0;
    targetMouseY = 0;
    tiltX = 0;
    tiltZ = 0;
    bootStartTime = Date.now();
    ledMats = [];
    ledLights = [];
    mouseMoveListener = (event) => {

        targetMouseX = (event.clientX / window.innerWidth) * 2 - 1;
        targetMouseY = -(event.clientY / window.innerHeight) * 2 + 1;
    };
    window.addEventListener('mousemove', mouseMoveListener);
    
    // Hemisphere light for natural ambient/ground lighting - calibrated for sRGB contrast
    const hemiLight = new THREE.HemisphereLight(0xffffff, 0x0f172a, 0.12);
    scene.add(hemiLight);
    
    // Main white key light from top-right-front - calibrated to prevent wash out
    const keyLight = new THREE.DirectionalLight(0xffffff, 0.5);
    keyLight.position.set(150, 250, 100);
    scene.add(keyLight);
    
    // Fill light with Indigo/Blue tint from top-left-back
    const fillLight = new THREE.DirectionalLight(0x93c5fd, 0.2);
    fillLight.position.set(-150, 100, -100);
    scene.add(fillLight);

    // Rim light from behind to pop edges
    const rimLight = new THREE.DirectionalLight(0xffffff, 0.6);
    rimLight.position.set(0, 150, -200);
    scene.add(rimLight);
    
    // Theme accent colored lights from front-left and front-right for spectacular metallic specular highlights
    // grazing angles used to highlight pins and components
    const accentLight1 = new THREE.DirectionalLight(0x00f0ff, 1.8); // neon cyan
    accentLight1.position.set(-15, 3, 10);
    scene.add(accentLight1);

    const accentLight2 = new THREE.DirectionalLight(0x0044ff, 1.4); // neon blue (removed pink to fix reddish tint)
    accentLight2.position.set(15, 3, 10);
    scene.add(accentLight2);
    
    // Group to hold both boards
    group = new THREE.Group();
    group.rotation.order = 'YXZ';
    group.scale.set(0, 0, 0);
    scene.add(group);

    
    // Loader
    const loader = new GLTFLoader();
    
    function prepareModel(model) {
        model.updateMatrixWorld(true);
        let meshCount = 0;
        
        model.traverse((child) => {
            if (child.isMesh) {
                meshCount++;
                child.castShadow = false;
                child.receiveShadow = false;
                
                if (child.geometry) {
                    if (!child.geometry.boundingBox) {
                        child.geometry.computeBoundingBox();
                    }
                    const localBox = child.geometry.boundingBox;
                    const size = new THREE.Vector3();
                    localBox.getSize(size);
                    
                    // Compute the true center in the model's coordinate space
                    const center = new THREE.Vector3();
                    localBox.getCenter(center);
                    child.localToWorld(center); // Convert from local mesh space to model world space
                    
                    const maxDim = Math.max(size.x, size.y, size.z);
                    const colorHex = child.material && child.material.color ? child.material.color.getHexString() : 'N/A';
                    
                    console.log(`[splash.js] Mesh ${meshCount}: Name="${child.name}", WorldCenter=(${center.x.toFixed(4)}, ${center.y.toFixed(4)}, ${center.z.toFixed(4)}), Size=(${size.x.toFixed(4)}, ${size.y.toFixed(4)}, ${size.z.toFixed(4)}), Color=#${colorHex}`);
                }
            }
        });
        
        console.log('[splash.js] Model Meshes Count:', meshCount);
        
        // Intelligent material shader customization based on sRGB color mapping
        model.traverse((child) => {
            if (child.isMesh && child.material) {
                const mats = Array.isArray(child.material) ? child.material : [child.material];
                mats.forEach(mat => {
                    if (mat.color) {
                        const hex = mat.color.getHexString().toLowerCase();
                        const sr = parseInt(hex.substring(0, 2), 16) / 255;
                        const sg = parseInt(hex.substring(2, 4), 16) / 255;
                        const sb = parseInt(hex.substring(4, 6), 16) / 255;
                        
                        // 1. Detect PCB substrate (green dominant in sRGB space)
                        if (sg > sr * 1.15 && sg > sb * 1.15 && sg > 0.12) {
                            // Differentiate flat PCB boards from tall 3D connector housings
                            // using the local geometry bounding box (invariant to world rotation/tilt)
                            if (child.geometry) {
                                if (!child.geometry.boundingBox) {
                                    child.geometry.computeBoundingBox();
                                }
                                const localBox = child.geometry.boundingBox;
                                const localSize = new THREE.Vector3();
                                localBox.getSize(localSize);
                                
                                if (localSize.y < 0.0035) {
                                    // Flat PCB substrate -> forest green
                                    mat.color.setHex(0x115232); 
                                    mat.roughness = 0.95; // highly rough matte surface to diffuse light smoothly
                                    mat.metalness = 0.0;  // purely non-metallic to avoid shiny specular highlights
                                    mat.transparent = false; // ensure full opacity
                                    mat.opacity = 1.0;
                                    if (mat.clearcoat !== undefined) mat.clearcoat = 0.0;
                                } else {
                                    // Tall connector housing -> matte black
                                    mat.color.setHex(0x181818);
                                    mat.roughness = 0.70;
                                    mat.metalness = 0.05;
                                    if (mat.clearcoat !== undefined) mat.clearcoat = 0.0;
                                }
                            }
                            mat.needsUpdate = true;
                        }
                        // 2. Detect Gold / Copper pins (gold/yellow dominant in sRGB space)
                        else if (sr > 0.45 && sg > 0.35 && sb < 0.65 && sr > sb * 1.3 && sg > sb * 1.1) {
                            // High-end gold material (lower metalness to render diffuse color in dark environment)
                            mat.roughness = 0.15;
                            mat.metalness = 0.40;
                            mat.color.setHex(0xe5a93b); 
                            if (mat.clearcoat !== undefined) {
                                mat.clearcoat = 1.0;
                                mat.clearcoatRoughness = 0.05;
                            }
                            mat.needsUpdate = true;
                        }
                        // 3. Detect Silver / Solder / Metal contacts (neutral grey and bright in sRGB space)
                        else if (Math.abs(sr - sg) < 0.08 && Math.abs(sg - sb) < 0.08 && sr > 0.35) {
                            // Solder/silver metal (lower metalness to render diffuse light)
                            mat.roughness = 0.20;
                            mat.metalness = 0.30;
                            if (mat.clearcoat !== undefined) {
                                mat.clearcoat = 0.5;
                                mat.clearcoatRoughness = 0.10;
                            }
                            mat.needsUpdate = true;
                        }
                        // 4. Detect Black / Dark Grey component bodies
                        else if (sr < 0.32 && sg < 0.32 && sb < 0.32) {
                            // Dark matte look for ICs, plastics, and sockets
                            mat.roughness = 0.70;
                            mat.metalness = 0.05;
                            // Make it slightly darker for better contrast
                            mat.color.multiplyScalar(0.6);
                            mat.needsUpdate = true;
                        }
                        // 5. General defaults for other components (LEDs, blue connectors, silkscreens)
                        else {
                            mat.roughness = 0.4;
                            mat.metalness = 0.1;
                        }
                    }
                });
            }
        });
    }



    function buildSceneWithModel(gltfScene) {
        if (isDestroyed) return;
        
        prepareModel(gltfScene);
        
        let size = new THREE.Vector3();
        let box = new THREE.Box3();
        let center = new THREE.Vector3();
        
        box.setFromObject(gltfScene);
        box.getSize(size);
        box.getCenter(center);
        
        console.log('[splash.js] Merged board model size:', size.x, size.y, size.z);
        console.log('[splash.js] Merged board model center:', center.x, center.y, center.z);
        
        // Center the merged board model in our group
        gltfScene.position.x = -center.x;
        gltfScene.position.y = -center.y;
        gltfScene.position.z = -center.z;
        
        // Initialize 3 custom SMD LEDs on bottom board (ESP32 mainboard)
        // Positioned next to the buttons (which are at x = -0.0336, z = 0.0320)
        const ledPositions = [
            { x: -0.02425, y: -0.0169, z: 0.03752 }, // LED 1
            { x: -0.02085, y: -0.0169, z: 0.03752 }, // LED 2
            { x: -0.01735, y: -0.0169, z: 0.03752 }  // LED 3
        ];
        
        // Extract native LED bodies from mesh_2 and get their materials
        ledMats = extractLEDs(gltfScene, ledPositions);
        ledLights = [];
        
        ledPositions.forEach((pos, idx) => {
            // Create two soft PointLights offset to the left and right pads to simulate side spill and ground the LED
            const lightL = new THREE.PointLight(0xffaa00, 0.0, 0.012, 2.0);
            lightL.position.set(pos.x - 0.0012, pos.y + 0.0002, pos.z); // offset 1.2mm left, 0.2mm above PCB
            gltfScene.add(lightL);
            ledLights.push(lightL);
            
            const lightR = new THREE.PointLight(0xffaa00, 0.0, 0.012, 2.0);
            lightR.position.set(pos.x + 0.0012, pos.y + 0.0002, pos.z); // offset 1.2mm right, 0.2mm above PCB
            gltfScene.add(lightR);
            ledLights.push(lightR);
        });
        
        group.add(gltfScene);



        
        // Compute combined size (just the single model)
        const maxDim = Math.max(size.x, size.y, size.z);
        
        // Position camera to fit the model occupying ~40% of screen height
        const fovRad = camera.fov * (Math.PI / 180);
        let cameraZ = Math.abs(maxDim / 2 / Math.tan(fovRad / 2));
        cameraZ = cameraZ / 0.40; // Aim for ~40% window height coverage
        
        console.log('[splash.js] maxDim:', maxDim, 'cameraZ:', cameraZ, 'window size:', window.innerWidth, window.innerHeight);
        
        // Tilt the camera at 24 degrees from horizontal (looking slightly down from top-front)
        // keeping the distance from target at exactly cameraZ.
        const tiltAngle = 24 * Math.PI / 180;
        const cameraY = cameraZ * Math.sin(tiltAngle);
        const cameraZCoord = cameraZ * Math.cos(tiltAngle);
        
        camera.position.set(0, cameraY, cameraZCoord);
        camera.lookAt(0, 0, 0);
        
        // Trigger resize calculation to set correct initial aspect and visible width
        updateCameraAndPositions();
        
        // Start animation loop
        animate();
    }

    console.log('[splash.js] Loading bugbuster.glb...');
    loader.load('./public/models/bugbuster.glb', (gltf) => {
        console.log('[splash.js] bugbuster.glb loaded successfully!');
        buildSceneWithModel(gltf.scene);
    }, undefined, (err) => {
        const errMsg = err?.message || err?.toString() || JSON.stringify(err);
        console.error('[splash.js] Failed to load bugbuster.glb:', errMsg);
    });
};

function animate() {
    if (isDestroyed) return;
    
    // Zoom in animation (scale from 0 to 1)
    scale += (1.0 - scale) * 0.04;
    group.scale.set(scale, scale, scale);
    
    // Rotate group
    group.rotation.y += 0.005;
    
    // Gyroscopic tilt (3D Parallax) towards mouse cursor - increased 3x
    const targetTiltX = mouseY * 0.18; // 3x increased: max ~10 degrees
    const targetTiltZ = -mouseX * 0.18;
    tiltX += (targetTiltX - tiltX) * 0.05;
    tiltZ += (targetTiltZ - tiltZ) * 0.05;
    
    // Combine Y rotation, X/Z wobble, and mouse-controlled gyroscopic tilt
    group.rotation.x = Math.sin(Date.now() * 0.0008) * 0.05 + tiltX;
    group.rotation.z = Math.cos(Date.now() * 0.0006) * 0.03 + tiltZ;
    
    // Smooth X position translation (shift left when scan done)
    currentX += (targetX - currentX) * 0.05;
    group.position.x = currentX;
    
    // Smoothly interpolate mouse coordinates for dynamic lighting
    mouseX += (targetMouseX - mouseX) * 0.08;
    mouseY += (targetMouseY - mouseY) * 0.08;
    
    // Update camera-relative point light position to track cursor
    if (cursorLight) {
        cursorLight.position.x = mouseX * 0.18;
        cursorLight.position.y = mouseY * 0.12;
    }
    
    // Animate LED boot sequence and PointLights
    const elapsed = Date.now() - bootStartTime;
    const pulse = 0.5 + 0.5 * Math.sin(elapsed * 0.004); // breathing pulse
    
    if (ledMats && ledMats.length >= 3 && ledLights && ledLights.length >= 3) {
        let t = 0;
        if (elapsed > 2000) {
            t = Math.min((elapsed - 2000) / 1000, 1.0); // 1-second transition
        }
        
        const yellowColor = new THREE.Color(0xffaa00);
        const greenColor = new THREE.Color(0x00ff44);
        
        // LED 1 & 2 (index 0 & 1): Yellow to Green
        for (let i = 0; i < 2; i++) {
            const mat = ledMats[i];
            const lightL = ledLights[i * 2];
            const lightR = ledLights[i * 2 + 1];
            if (mat && lightL && lightR) {
                const activeColor = yellowColor.clone().lerp(greenColor, t);
                mat.emissive.copy(activeColor);
                
                lightL.color.copy(activeColor);
                lightR.color.copy(activeColor);
                
                // Interpolate diffuse color so the plastic body changes color dynamically
                const baseColor = new THREE.Color(0xdddddd).lerp(activeColor, 0.6);
                mat.color.copy(baseColor);
                
                const startIntensity = pulse * 1.5;
                const finalIntensity = startIntensity + (1.2 - startIntensity) * t;
                mat.emissiveIntensity = finalIntensity;
                
                // Split the target intensity between the left and right pad lights
                const intensityVal = finalIntensity * 0.0225;
                lightL.intensity = intensityVal;
                lightR.intensity = intensityVal;
            }
        }
        
        // LED 3 (index 2): Yellow (stays yellow)
        const mat3 = ledMats[2];
        const light3L = ledLights[4];
        const light3R = ledLights[5];
        if (mat3 && light3L && light3R) {
            mat3.emissive.copy(yellowColor);
            
            light3L.color.copy(yellowColor);
            light3R.color.copy(yellowColor);
            
            // Interpolate diffuse color for LED 3
            const baseColor3 = new THREE.Color(0xdddddd).lerp(yellowColor, 0.6);
            mat3.color.copy(baseColor3);
            
            const startIntensity = pulse * 1.5;
            const finalIntensity = startIntensity + (1.2 - startIntensity) * t;
            mat3.emissiveIntensity = finalIntensity;
            
            const intensityVal = finalIntensity * 0.0225;
            light3L.intensity = intensityVal;
            light3R.intensity = intensityVal;
        }
    }
    
    renderer.render(scene, camera);
    animationFrameId = requestAnimationFrame(animate);
}



function updateCameraAndPositions() {
    if (!renderer || !camera) return;
    
    const width = window.innerWidth;
    const height = window.innerHeight;
    
    camera.aspect = width / height;
    camera.updateProjectionMatrix();
    renderer.setSize(width, height);
    
    // Calculate visible width at camera's distance to target (0,0,0)
    const vFOV = camera.fov * Math.PI / 180;
    const visibleHeight = 2 * Math.tan(vFOV / 2) * camera.position.length();
    const visibleWidth = visibleHeight * camera.aspect;
    
    if (isScanCompleted) {
        targetX = -visibleWidth / 4.0; // Shift to the center of the left half
    } else {
        targetX = 0; // Centered
    }
}

window.addEventListener('resize', updateCameraAndPositions);

window.updateScanStatus = function(completed) {
    isScanCompleted = completed;
    updateCameraAndPositions();
};

window.destroySplash = function() {
    isDestroyed = true;
    if (animationFrameId) {
        cancelAnimationFrame(animationFrameId);
        animationFrameId = null;
    }
    window.removeEventListener('resize', updateCameraAndPositions);
    
    if (mouseMoveListener) {
        window.removeEventListener('mousemove', mouseMoveListener);
        mouseMoveListener = null;
    }
    
    if (renderer) {
        renderer.dispose();
        renderer = null;
    }
    
    if (scene) {
        scene.traverse((object) => {
            if (!object.isMesh) return;
            
            if (object.geometry) {
                object.geometry.dispose();
            }
            
            if (object.material) {
                if (Array.isArray(object.material)) {
                    object.material.forEach(material => material.dispose());
                } else {
                    object.material.dispose();
                }
            }
        });
        scene = null;
    }
    
    camera = null;
    group = null;
    cursorLight = null;
    ledMats = [];
    ledLights = [];
};


