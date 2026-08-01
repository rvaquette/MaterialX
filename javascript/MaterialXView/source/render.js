//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//
// Headless rendering entry point.
//
// Loads a geometry file (.obj or .glb) and an environment (.hdr), renders the
// scene once into an offscreen canvas at a requested resolution, and exposes
// the result so an external driver (e.g. Playwright) can save it to disk.
//
// Query parameters:
//   geom  : geometry file URL (.obj or .glb)   default: Geometry/sphere.obj
//   env   : environment HDR URL                 default: Lights/san_giuseppe_bridge_split.hdr
//   file  : MaterialX material file (.mtlx)     default: standard_surface_default
//   width : output image width  (px)            default: 1024
//   height: output image height (px)            default: 1024
//

import * as THREE from 'three';
import { Viewer } from './viewer.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';

const params = new URLSearchParams(document.location.search);
const geometryURL = params.get('geom') || 'Geometry/sphere.obj';
const envURL = params.get('env') || 'Lights/san_giuseppe_bridge_split.hdr';
// Diffuse irradiance map. Defaults to the pre-convolved map that lives next to
// the radiance map under a sibling `irradiance/` folder, matching the viewer
// (index.html). Can be overridden with the `envIrradiance` query parameter.
const deriveIrradianceURL = (radianceURL) =>
{
    const slash = radianceURL.lastIndexOf('/');
    const dir = slash === -1 ? '' : radianceURL.slice(0, slash + 1);
    const base = slash === -1 ? radianceURL : radianceURL.slice(slash + 1);
    return `${dir}irradiance/${base}`;
};
const envIrradianceURL = params.get('envIrradiance') || deriveIrradianceURL(envURL);
const materialFilename = params.get('file') || 'Materials/Examples/StandardSurface/standard_surface_default.mtlx';
const width = parseInt(params.get('width'), 10) || 1024;
const height = parseInt(params.get('height'), 10) || 1024;

// Signal flags consumed by the headless driver.
window.renderComplete = false;
window.renderError = null;

const viewer = Viewer.create();

async function renderToImage()
{
    const canvas = document.getElementById('webglcanvas');
    canvas.width = width;
    canvas.height = height;

    const renderer = new THREE.WebGLRenderer({ antialias: true, canvas, preserveDrawingBuffer: true });
    renderer.setSize(width, height);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    renderer.debug.checkShaderErrors = false;

    const scene = viewer.getScene();
    scene.initialize();
    scene.setGeometryURL(geometryURL);

    const orbitControls = new OrbitControls(scene.getCamera(), renderer.domElement);

    viewer.getEditor().initialize();

    const hdrLoader = viewer.getHdrLoader();
    const fileLoader = viewer.getFileLoader();

    const [radianceTexture, irradianceTexture, lightRigXml, mxIn] = await Promise.all([
        new Promise(resolve => hdrLoader.load(envURL, resolve)),
        new Promise(resolve => hdrLoader.load(envIrradianceURL, resolve)),
        new Promise(resolve => fileLoader.load('Lights/san_giuseppe_bridge_split.mtlx', resolve)),
        import(/* webpackIgnore: true */ './JsMaterialXGenShader.js')
            .then(({ default: MaterialX }) => MaterialX())
    ]);

    // Initialize viewer + lighting.
    await viewer.initialize(mxIn, renderer, radianceTexture, irradianceTexture, lightRigXml);

    // initializeLighting is fired asynchronously inside initialize(); wait for the
    // radiance/irradiance environment textures to be prepared before generating materials.
    while (!viewer.getRadianceTexture() || !viewer.getIrradianceTexture())
    {
        await new Promise(r => setTimeout(r, 20));
    }

    // Load geometry and materials.
    await scene.loadGeometry(viewer, orbitControls);
    await viewer.getMaterial().loadMaterials(viewer, materialFilename);
    viewer.getMaterial().updateMaterialAssignments(viewer);

    // Fix the camera aspect ratio to the requested image dimensions.
    const camera = scene.getCamera();
    camera.aspect = width / height;
    camera.updateProjectionMatrix();

    // Collect camera information (position, lookAt target, field of view).
    const target = orbitControls.target;
    window.cameraInfo = {
        position: { x: camera.position.x, y: camera.position.y, z: camera.position.z },
        lookAt: { x: target.x, y: target.y, z: target.z },
        fov: camera.fov,
        aspect: camera.aspect,
        near: camera.near,
        far: camera.far,
        // Photographic parameters. THREE.PerspectiveCamera exposes the focus
        // distance and film gauge; aperture is only present if explicitly set.
        focalDistance: camera.focus,
        focalLength: camera.getFocalLength(),
        filmGauge: camera.filmGauge,
        aperture: (camera.aperture !== undefined ? camera.aperture : null)
    };
    console.log('Camera info:', JSON.stringify(window.cameraInfo));

    // Collect the generated GLSL source code from the assigned surface shaders.
    // The viewer builds a THREE.RawShaderMaterial whose vertexShader/fragmentShader
    // hold the MaterialX-generated GLSL for each renderable mesh.
    const glslSources = [];
    const seen = new Set();
    scene.getScene().traverse((child) =>
    {
        if (child.isMesh && child.material && child.material.fragmentShader)
        {
            const name = child.material.name || child.name || `shader_${glslSources.length}`;
            if (!seen.has(name))
            {
                seen.add(name);
                glslSources.push({
                    name,
                    vertex: child.material.vertexShader,
                    fragment: child.material.fragmentShader
                });
            }
        }
    });
    window.glslSources = glslSources;
    console.log(`Generated GLSL shaders: ${glslSources.map(s => s.name).join(', ') || 'none'}`);

    // Render a few frames so the environment mipmaps and uniforms are fully
    // resolved before the frame is captured.
    for (let i = 0; i < 5; i++)
    {
        scene.updateTimeUniforms();
        renderer.render(scene.getScene(), camera);
        await new Promise(r => requestAnimationFrame(r));
    }

    window.renderComplete = true;
}

renderToImage().catch(err =>
{
    const message = Number.isInteger(err) ? viewer.getMx().getExceptionMessage(err) : (err && err.message ? err.message : String(err));
    console.error(message);
    window.renderError = message;
});
