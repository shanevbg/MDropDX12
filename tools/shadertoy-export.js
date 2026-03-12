// Shadertoy → MDropDX12 shader_import JSON exporter
// Usage: Open a shader on shadertoy.com, open browser DevTools console (F12),
//        paste this entire script and press Enter.
//        The JSON will be copied to clipboard and downloaded as a .json file.
//
// Uses gShaderToy.mEffect.mPasses[] for per-pass source code (mSource),
// and gShaderToy.Save() for metadata (inputs, outputs, type).
// gShaderToy.Save().code only returns the active tab — mSource has each pass's own code.

(function() {
    'use strict';

    if (typeof gShaderToy === 'undefined') {
        console.error('ERROR: gShaderToy not found. Make sure you are on a Shadertoy shader page.');
        return;
    }

    const effect = gShaderToy.mEffect;
    if (!effect || !effect.mPasses || effect.mPasses.length === 0) {
        console.error('ERROR: No passes found in gShaderToy.mEffect.mPasses');
        return;
    }

    // Also get the serialized data for metadata (inputs, outputs)
    const data = gShaderToy.Save();
    if (!data || !data.renderpass) {
        console.error('ERROR: gShaderToy.Save() returned no renderpass data');
        return;
    }

    // MDropDX12 ChannelSource enum values
    const CHAN = {
        NOISE_LQ:    0,  NOISE_MQ:    1,  NOISE_HQ:    2,
        FEEDBACK:    3,  NOISEVOL_LQ: 4,  NOISEVOL_HQ: 5,
        IMAGE_PREV:  6,  AUDIO:       7,  RANDOM_TEX:  8,
        BUFFER_B:    9,  BUFFER_C:   10,  BUFFER_D:   11,
    };

    // Shadertoy buffer output IDs → buffer index (A=0, B=1, C=2, D=3)
    const BUFFER_ID = { 257: 0, 258: 1, 259: 2, 260: 3 };
    const BUFFER_NAMES = ['Buffer A', 'Buffer B', 'Buffer C', 'Buffer D'];
    const BUFFER_CHAN  = [CHAN.FEEDBACK, CHAN.BUFFER_B, CHAN.BUFFER_C, CHAN.BUFFER_D];

    // Map a Shadertoy input to our ChannelSource enum
    function resolveChannel(input) {
        if (!input) return CHAN.NOISE_LQ;

        const ctype = input.ctype;
        const id = typeof input.id === 'string' ? parseInt(input.id) : input.id;
        const src = (input.src || input.filepath || '').toLowerCase();

        if (ctype === 'buffer' && id in BUFFER_ID) return BUFFER_CHAN[BUFFER_ID[id]];
        if (ctype === 'keyboard') return CHAN.NOISE_LQ;
        if (ctype === 'music' || ctype === 'musicstream' || ctype === 'mic') return CHAN.AUDIO;
        if (ctype === 'texture') {
            if (src.includes('medium') || src.includes('rgba01')) return CHAN.NOISE_MQ;
            if (src.includes('small') || src.includes('rgba00')) return CHAN.NOISE_LQ;
            if (src.includes('noise') || src.includes('rgba')) return CHAN.NOISE_HQ;
            if (src.includes('organic') || src.includes('abstract')) return CHAN.RANDOM_TEX;
            return CHAN.NOISE_LQ;
        }
        if (ctype === 'volume') {
            return src.includes('grey') || src.includes('gray') ? CHAN.NOISEVOL_LQ : CHAN.NOISEVOL_HQ;
        }
        if (ctype === 'cubemap') return CHAN.NOISE_HQ;
        return CHAN.NOISE_LQ;
    }

    // Get pass name from renderpass metadata
    function getPassName(rp) {
        if (rp.type === 'image') return 'Image';
        if (rp.type === 'common') return 'Common';
        if (rp.type === 'buffer') {
            if (rp.outputs && rp.outputs.length > 0) {
                const outId = typeof rp.outputs[0].id === 'string'
                    ? parseInt(rp.outputs[0].id) : rp.outputs[0].id;
                if (outId in BUFFER_ID) return BUFFER_NAMES[BUFFER_ID[outId]];
            }
            const n = (rp.name || '').trim();
            if (n.startsWith('Buf ')) return 'Buffer ' + n.charAt(4);
            return null;
        }
        return null;
    }

    // Extract source code from a live pass object, trying multiple property paths
    function getPassSource(passObj) {
        // Try direct source property
        if (passObj.mSource && typeof passObj.mSource === 'string' && passObj.mSource.length > 0)
            return passObj.mSource;
        // Try CodeMirror document (mDocs is array of CodeMirror docs)
        if (passObj.mDocs && passObj.mDocs.length > 0) {
            try {
                const val = passObj.mDocs[0].getValue();
                if (val && val.length > 0) return val;
            } catch(e) {}
        }
        // Try mCode
        if (passObj.mCode && typeof passObj.mCode === 'string' && passObj.mCode.length > 0)
            return passObj.mCode;
        return '';
    }

    // Diagnostic: log what properties each pass has
    console.log('%cPass object properties:', 'color: #aaa');
    for (let i = 0; i < effect.mPasses.length; i++) {
        const p = effect.mPasses[i];
        const keys = Object.keys(p).filter(k => typeof p[k] === 'string' || typeof p[k] === 'object');
        const srcLen = getPassSource(p).length;
        const type = data.renderpass[i] ? data.renderpass[i].type : '?';
        console.log(`  [${i}] type=${type} src=${srcLen} chars, keys: ${keys.join(', ')}`);
    }

    // Build output passes — match live passes to Save() metadata by index
    const passes = [];
    const count = Math.min(effect.mPasses.length, data.renderpass.length);
    for (let i = 0; i < count; i++) {
        const rp = data.renderpass[i];
        const name = getPassName(rp);
        if (!name) continue;

        // Get code from the live pass object, NOT from Save() which may have stale/duplicated code
        let glsl = getPassSource(effect.mPasses[i]);
        // Fallback to Save() data only if live source is empty
        if (!glsl) glsl = rp.code || '';

        const notes = rp.name || '';

        // Parse channel inputs from the serialized metadata
        const channels = {};
        if (rp.inputs) {
            for (const inp of rp.inputs) {
                const ch = inp.channel;
                if (ch >= 0 && ch <= 3) {
                    channels['ch' + ch] = resolveChannel(inp);
                }
            }
        }
        for (let ci = 0; ci < 4; ci++) {
            const key = 'ch' + ci;
            if (!(key in channels)) {
                channels[key] = [CHAN.NOISE_LQ, CHAN.NOISE_LQ, CHAN.NOISE_MQ, CHAN.NOISE_HQ][ci];
            }
        }

        passes.push({ name, glsl, notes, channels });
    }

    // Reorder: Image, Common, Buffer A, B, C, D
    const order = ['Image', 'Common', 'Buffer A', 'Buffer B', 'Buffer C', 'Buffer D'];
    passes.sort((a, b) => order.indexOf(a.name) - order.indexOf(b.name));

    const output = { type: 'shader_import', version: 1, passes };
    const json = JSON.stringify(output, null, 2);

    // Summary
    const shaderName = data.info ? data.info.name : 'unknown';
    console.log(`%cShadertoy Export: ${shaderName}`, 'color: #0af; font-weight: bold; font-size: 14px');
    for (const p of passes) {
        const first50 = p.glsl.substring(0, 50).replace(/\n/g, ' ');
        console.log(`  %c${p.name}: ${p.glsl.length} chars — "${first50}..."`, 'color: #aaa');
    }

    // Warn about empty or duplicate passes
    const empty = passes.filter(p => p.name !== 'Common' && p.glsl.length === 0);
    if (empty.length > 0) {
        console.warn(`%c⚠ Empty GLSL in: ${empty.map(p => p.name).join(', ')}`, 'color: #ff0; font-weight: bold');
    }
    // Check for duplicates (same code in different passes = the bug we're fixing)
    for (let i = 0; i < passes.length; i++) {
        for (let j = i + 1; j < passes.length; j++) {
            if (passes[i].glsl.length > 100 && passes[i].glsl === passes[j].glsl) {
                console.warn(`%c⚠ DUPLICATE: ${passes[i].name} and ${passes[j].name} have identical code!`,
                    'color: #f00; font-weight: bold');
            }
        }
    }

    // Copy to clipboard
    navigator.clipboard.writeText(json).then(() => {
        console.log('%c✓ JSON copied to clipboard!', 'color: #0f0; font-weight: bold; font-size: 14px');
    }).catch(() => {
        console.log('%c✗ Could not copy to clipboard — use the download instead', 'color: #f00; font-weight: bold');
    });

    // Trigger download
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    const safeName = (shaderName || 'shader').replace(/[^a-zA-Z0-9_-]/g, '');
    a.download = safeName + '.json';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
    console.log(`%c✓ Downloaded: ${a.download}`, 'color: #0f0');
})();
