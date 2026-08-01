//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//
// Headless render CLI.
//
// Loads a geometry (.obj/.glb) and an environment (.hdr) in the MaterialX
// viewer, renders one frame at a requested resolution and writes a PNG to disk.
//
// Prerequisites: run `npm run build` first so that `dist/` is populated.
//
// Usage:
//   node scripts/renderImage.mjs --geom Geometry/teapot.obj --env Lights/goegap.hdr \
//        --width 1024 --height 1024 --out render.png [--mtlx Materials/.../foo.mtlx] \
//        [--envIrradiance Lights/irradiance/goegap.hdr]
//
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from '@playwright/test';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const distDir = path.resolve(__dirname, '..', 'dist');

function arg(name, def)
{
    const i = process.argv.indexOf(`--${name}`);
    return i !== -1 && process.argv[i + 1] ? process.argv[i + 1] : def;
}

const geom = arg('geom', 'Geometry/shaderball.glb');
const env = arg('env', 'Lights/san_giuseppe_bridge_split.hdr');
const envIrradiance = arg('envIrradiance', '');
const file = arg('mtlx', arg('file', 'Materials/Examples/StandardSurface/standard_surface_default.mtlx'));
const width = parseInt(arg('width', '1024'), 10);
const height = parseInt(arg('height', '1024'), 10);
const out = path.resolve(arg('out', 'render.png'));

const mimeTypes = {
    '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
    '.json': 'application/json', '.wasm': 'application/wasm', '.data': 'application/octet-stream',
    '.hdr': 'application/octet-stream', '.mtlx': 'application/xml', '.glb': 'model/gltf-binary',
    '.obj': 'text/plain', '.png': 'image/png', '.jpg': 'image/jpeg', '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon'
};

const server = http.createServer((req, res) =>
{
    const urlPath = decodeURIComponent(req.url.split('?')[0]);
    const filePath = path.join(distDir, urlPath === '/' ? 'index.html' : urlPath);
    if (!filePath.startsWith(distDir) || !fs.existsSync(filePath))
    {
        res.writeHead(404).end('Not found');
        return;
    }
    res.writeHead(200, { 'Content-Type': mimeTypes[path.extname(filePath)] || 'application/octet-stream' });
    fs.createReadStream(filePath).pipe(res);
});

await new Promise(resolve => server.listen(0, resolve));
const port = server.address().port;
const url = `http://localhost:${port}/render.html?geom=${encodeURIComponent(geom)}&env=${encodeURIComponent(env)}${envIrradiance ? `&envIrradiance=${encodeURIComponent(envIrradiance)}` : ''}&file=${encodeURIComponent(file)}&width=${width}&height=${height}`;

const browser = await chromium.launch({
    args: ['--use-gl=angle', '--use-angle=d3d11', '--ignore-gpu-blocklist', '--enable-gpu', '--enable-unsafe-webgpu']
});
const page = await browser.newPage({ viewport: { width, height } });
page.on('console', msg => console.log('[page]', msg.text()));

try
{
    await page.goto(url);
    await page.waitForFunction(() => window.renderComplete || window.renderError, null, { timeout: 120000 });
    const error = await page.evaluate(() => window.renderError);
    if (error) throw new Error(error);

    const dataUrl = await page.evaluate(() => document.getElementById('webglcanvas').toDataURL('image/png'));
    fs.writeFileSync(out, Buffer.from(dataUrl.split(',')[1], 'base64'));
    console.log(`Saved ${width}x${height} render to ${out}`);

    // Export the generated GLSL surface shaders next to the output image.
    const glslSources = await page.evaluate(() => window.glslSources || []);
    const outDir = path.dirname(out);
    const outBase = path.basename(out, path.extname(out));
    for (const shader of glslSources)
    {
        const safeName = shader.name.replace(/[^A-Za-z0-9_.-]/g, '_');
        const vertPath = path.join(outDir, `${outBase}.${safeName}.vert.glsl`);
        const fragPath = path.join(outDir, `${outBase}.${safeName}.frag.glsl`);
        fs.writeFileSync(vertPath, shader.vertex);
        fs.writeFileSync(fragPath, shader.fragment);
        console.log(`Exported GLSL for '${shader.name}': ${vertPath}, ${fragPath}`);
    }
    if (glslSources.length === 0)
    {
        console.log('No GLSL surface shaders were generated.');
    }
}
finally
{
    await browser.close();
    server.close();
}
