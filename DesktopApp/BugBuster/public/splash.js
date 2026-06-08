import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/GLTFLoader.js';

let scene, camera, renderer, animationFrameId;
let group;
let scale = 0;
let targetX = 0;
let currentX = 0;
let isScanCompleted = false;
let isDestroyed = false;

// Cursor tracking variables for dynamic lighting
let mouseX = 0;
let mouseY = 0;
let targetMouseX = 0;
let targetMouseY = 0;
let cursorLight = null;
let mouseMoveListener = null;

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
    
    // Set up mouse listener
    mouseX = 0;
    mouseY = 0;
    targetMouseX = 0;
    targetMouseY = 0;
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
    group.scale.set(0, 0, 0);
    scene.add(group);
    
    // Loader
    const loader = new GLTFLoader();
    
    function prepareModel(model) {
        model.updateMatrixWorld(true);
        const uniqueMaterials = new Map();
        let meshCount = 0;
        
        model.traverse((child) => {
            if (child.isMesh) {
                meshCount++;
                child.castShadow = false;
                child.receiveShadow = false;
                if (child.material) {
                    const mats = Array.isArray(child.material) ? child.material : [child.material];
                    mats.forEach(mat => {
                        if (!uniqueMaterials.has(mat.uuid)) {
                            uniqueMaterials.set(mat.uuid, {
                                name: mat.name,
                                color: mat.color ? mat.color.getHexString() : 'N/A',
                                roughness: mat.roughness,
                                metalness: mat.metalness,
                                map: !!mat.map,
                                type: mat.type
                            });
                        }
                    });
                }
            }
        });
        
        console.log('[splash.js] Model Meshes Count:', meshCount);
        console.log('[splash.js] Model Unique Materials Count:', uniqueMaterials.size);
        
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
                            const box = new THREE.Box3().setFromObject(child);
                            const size = new THREE.Vector3();
                            box.getSize(size);
                            
                            if (size.y < 0.0035) {
                                // Flat PCB substrate -> forest green
                                mat.color.setHex(0x115232); 
                                mat.roughness = 0.80;
                                mat.metalness = 0.10;
                                if (mat.clearcoat !== undefined) mat.clearcoat = 0.0;
                            } else {
                                // Tall connector housing -> matte black
                                mat.color.setHex(0x181818);
                                mat.roughness = 0.70;
                                mat.metalness = 0.05;
                                if (mat.clearcoat !== undefined) mat.clearcoat = 0.0;
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

    console.log('[splash.js] Loading Bug buster.glb...');
    loader.load('./public/models/Bug buster.glb', (gltf) => {
        console.log('[splash.js] Bug buster.glb loaded successfully!');
        buildSceneWithModel(gltf.scene);
    }, undefined, (err) => {
        const errMsg = err?.message || err?.toString() || JSON.stringify(err);
        console.warn('[splash.js] Failed to load uncompressed model, trying compressed fallback:', errMsg);
        loader.load('./public/models/Bug buster comp.glb', (gltf) => {
            console.log('[splash.js] Bug buster comp.glb (fallback) loaded successfully!');
            buildSceneWithModel(gltf.scene);
        }, undefined, (fallbackErr) => {
            const fallbackErrMsg = fallbackErr?.message || fallbackErr?.toString() || JSON.stringify(fallbackErr);
            console.error('[splash.js] Failed to load compressed fallback model:', fallbackErrMsg);
        });
    });
};

function animate() {
    if (isDestroyed) return;
    
    // Zoom in animation (scale from 0 to 1)
    scale += (1.0 - scale) * 0.04;
    group.scale.set(scale, scale, scale);
    
    // Rotate group
    group.rotation.y += 0.005;
    // Add subtle tilt wobble
    group.rotation.x = Math.sin(Date.now() * 0.0008) * 0.08;
    group.rotation.z = Math.cos(Date.now() * 0.0006) * 0.04;
    
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
};
