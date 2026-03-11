#!/usr/bin/env node
// MCP Server for MDropDX12 Named Pipe IPC
// Lets Claude Code interact with the running visualizer.

import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';
import net from 'net';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { execSync } from 'child_process';

// ── Pipe helpers ──

function discoverPipes() {
  try {
    const pipeDir = '//./pipe/';
    const pipes = fs.readdirSync(pipeDir)
      .filter(name => name.startsWith('Milkwave_'))
      .map(name => {
        const match = name.match(/Milkwave_(\d+)/);
        return match ? { path: pipeDir + name, pid: parseInt(match[1]) } : null;
      })
      .filter(Boolean);

    // If multiple pipes found, prefer the MDropDX12 process over Milkwave Visualizer
    if (pipes.length > 1) {
      try {
        const output = execSync('tasklist /fo csv /nh', { encoding: 'utf8', timeout: 5000 });
        const mdropPids = new Set();
        for (const line of output.split(/\r?\n/)) {
          if (line.toLowerCase().includes('mdropdx12')) {
            const pidMatch = line.match(/"(\d+)"/);
            if (pidMatch) mdropPids.add(parseInt(pidMatch[1]));
          }
        }
        if (mdropPids.size > 0) {
          const mdropPipes = pipes.filter(p => mdropPids.has(p.pid));
          if (mdropPipes.length > 0) return mdropPipes;
        }
      } catch { /* fall through to return all pipes */ }
    }

    return pipes;
  } catch {
    return [];
  }
}

function sendPipeMessage(pipePath, message, expectResponse = false) {
  return new Promise((resolve, reject) => {
    const client = net.connect(pipePath, () => {
      // Send as UTF-16LE with null terminator
      const buf = Buffer.from(message + '\0', 'utf16le');
      client.write(buf);

      if (!expectResponse) {
        // Give the pipe a moment to flush, then close
        setTimeout(() => {
          client.end();
          resolve('OK');
        }, 50);
        return;
      }

      // Collect response data
      const chunks = [];
      let timer = null;

      const finish = () => {
        if (timer) clearTimeout(timer);
        client.end();
        const data = Buffer.concat(chunks);
        // Decode UTF-16LE response, strip nulls
        const text = data.toString('utf16le').replace(/\0/g, '');
        resolve(text);
      };

      client.on('data', (chunk) => {
        chunks.push(chunk);
        // Reset timer on each data chunk (responses come in bursts)
        if (timer) clearTimeout(timer);
        timer = setTimeout(finish, 300);
      });

      // If no data within 1s, resolve with empty
      timer = setTimeout(finish, 1000);
    });

    client.on('error', (err) => {
      reject(new Error(`Pipe connection failed: ${err.message}`));
    });
  });
}

// ── Cached connection ──

let cachedPipePath = null;

async function ensureConnected() {
  // Test if cached pipe is still valid
  if (cachedPipePath) {
    try {
      await sendPipeMessage(cachedPipePath, 'STATE', true);
      return cachedPipePath;
    } catch {
      cachedPipePath = null;
    }
  }

  const pipes = discoverPipes();
  if (pipes.length === 0) {
    throw new Error('No running MDropDX12 instance found. Start the visualizer first.');
  }

  // Try first discovered pipe
  cachedPipePath = pipes[0].path;
  return cachedPipePath;
}

async function send(message, expectResponse = false) {
  const pipePath = await ensureConnected();
  return sendPipeMessage(pipePath, message, expectResponse);
}

// ── Capture helpers ──

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(__dirname, '../..');

function findCaptureDir() {
  if (process.env.MDROP_CAPTURE_DIR) {
    return process.env.MDROP_CAPTURE_DIR;
  }
  const releasePath = path.join(projectRoot, 'src/mDropDX12/Release_x64/capture');
  if (fs.existsSync(releasePath)) return releasePath;
  const debugPath = path.join(projectRoot, 'src/mDropDX12/Debug_x64/capture');
  if (fs.existsSync(debugPath)) return debugPath;
  return releasePath; // fallback — visualizer will create it
}

function waitForNewCapture(dir, afterTimestamp, timeoutMs) {
  return new Promise((resolve) => {
    const pollInterval = 100;
    let elapsed = 0;

    const check = () => {
      if (elapsed >= timeoutMs) { resolve(null); return; }
      try {
        if (fs.existsSync(dir)) {
          const files = fs.readdirSync(dir)
            .filter(f => f.endsWith('.png'))
            .map(f => {
              const fullPath = path.join(dir, f);
              const stat = fs.statSync(fullPath);
              return { path: fullPath, mtime: stat.mtimeMs };
            })
            .filter(f => f.mtime >= afterTimestamp - 1000)
            .sort((a, b) => b.mtime - a.mtime);

          if (files.length > 0 && fs.statSync(files[0].path).size > 0) {
            resolve(files[0].path);
            return;
          }
        }
      } catch { /* dir may not exist yet */ }
      elapsed += pollInterval;
      setTimeout(check, pollInterval);
    };

    setTimeout(check, 50);
  });
}

// ── MCP Server ──

const server = new McpServer({
  name: 'mdrop',
  version: '1.0.0',
});

// Tool: Connect / discover
server.tool(
  'mdrop_connect',
  'Discover and connect to a running MDropDX12 instance',
  {},
  async () => {
    const pipes = discoverPipes();
    if (pipes.length === 0) {
      return { content: [{ type: 'text', text: 'No running MDropDX12 instance found. Start the visualizer first.' }] };
    }
    cachedPipePath = pipes[0].path;
    const pids = pipes.map(p => p.pid).join(', ');
    return { content: [{ type: 'text', text: `Connected to MDropDX12 (PID: ${pids}), pipe: ${cachedPipePath}` }] };
  }
);

// Tool: Query state
server.tool(
  'mdrop_state',
  'Query current visualizer state (preset, opacity, quality, colors, FFT, etc.)',
  {},
  async () => {
    try {
      const response = await send('STATE', true);
      return { content: [{ type: 'text', text: response || '(no response — visualizer may not support STATE query)' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Load preset
server.tool(
  'mdrop_load_preset',
  'Load a preset by filename or full path',
  { preset: z.string().describe('Preset filename (e.g. "MyPreset.milk") or full path') },
  async ({ preset }) => {
    try {
      await send(`PRESET=${preset}`);
      return { content: [{ type: 'text', text: `Loaded preset: ${preset}` }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Next preset
server.tool(
  'mdrop_next_preset',
  'Switch to the next preset',
  {},
  async () => {
    try {
      await send('SIGNAL|NEXT_PRESET');
      return { content: [{ type: 'text', text: 'Switched to next preset' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Previous preset
server.tool(
  'mdrop_prev_preset',
  'Switch to the previous preset',
  {},
  async () => {
    try {
      await send('SIGNAL|PREV_PRESET');
      return { content: [{ type: 'text', text: 'Switched to previous preset' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Send text message
server.tool(
  'mdrop_send_message',
  'Display a text message on the visualizer',
  {
    text: z.string().describe('Message text to display'),
    font: z.string().optional().describe('Font name (default: Arial)'),
    size: z.number().optional().describe('Font size (default: 32)'),
    r: z.number().optional().describe('Red 0-255 (default: 255)'),
    g: z.number().optional().describe('Green 0-255 (default: 255)'),
    b: z.number().optional().describe('Blue 0-255 (default: 255)'),
    duration: z.number().optional().describe('Display duration in seconds (default: 5)'),
  },
  async ({ text, font, size, r, g, b, duration }) => {
    try {
      let msg = `MSG|text=${text}`;
      if (font) msg += `|font=${font}`;
      msg += `|size=${size || 32}`;
      msg += `|r=${r ?? 255}|g=${g ?? 255}|b=${b ?? 255}`;
      msg += `|time=${duration || 5}`;
      await send(msg);
      return { content: [{ type: 'text', text: `Message sent: "${text}"` }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Set color
server.tool(
  'mdrop_set_color',
  'Adjust hue, saturation, and/or brightness',
  {
    hue: z.number().min(0).max(1).optional().describe('Hue shift 0.0-1.0'),
    saturation: z.number().min(-1).max(1).optional().describe('Saturation -1.0 to 1.0'),
    brightness: z.number().min(-1).max(1).optional().describe('Brightness -1.0 to 1.0'),
  },
  async ({ hue, saturation, brightness }) => {
    try {
      const results = [];
      if (hue !== undefined) { await send(`COL_HUE=${hue}`); results.push(`hue=${hue}`); }
      if (saturation !== undefined) { await send(`COL_SATURATION=${saturation}`); results.push(`sat=${saturation}`); }
      if (brightness !== undefined) { await send(`COL_BRIGHTNESS=${brightness}`); results.push(`brt=${brightness}`); }
      return { content: [{ type: 'text', text: results.length ? `Set: ${results.join(', ')}` : 'No parameters specified' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Capture screenshot
server.tool(
  'mdrop_capture',
  'Take a screenshot of the current visualizer output and return the image',
  {
    return_image: z.boolean().optional().describe('Return the PNG image data (default: true). Set false for just confirmation.'),
  },
  async ({ return_image }) => {
    try {
      const captureDir = findCaptureDir();
      const beforeTime = Date.now();

      await send('SIGNAL|CAPTURE');

      if (return_image === false) {
        return { content: [{ type: 'text', text: 'Screenshot capture triggered' }] };
      }

      const filePath = await waitForNewCapture(captureDir, beforeTime, 3000);

      if (!filePath) {
        return { content: [{ type: 'text', text: 'Screenshot captured but file not found within timeout. Check capture/ directory.' }] };
      }

      const imageData = fs.readFileSync(filePath);
      const base64 = imageData.toString('base64');

      return {
        content: [
          { type: 'image', data: base64, mimeType: 'image/png' },
          { type: 'text', text: `Saved: ${path.basename(filePath)}` },
        ]
      };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Set FFT parameters
server.tool(
  'mdrop_set_fft',
  'Adjust FFT attack and decay smoothing',
  {
    attack: z.number().min(0).max(1).optional().describe('FFT attack 0.0-1.0'),
    decay: z.number().min(0).max(1).optional().describe('FFT decay 0.0-1.0'),
  },
  async ({ attack, decay }) => {
    try {
      const results = [];
      if (attack !== undefined) { await send(`FFT_ATTACK=${attack}`); results.push(`attack=${attack}`); }
      if (decay !== undefined) { await send(`FFT_DECAY=${decay}`); results.push(`decay=${decay}`); }
      return { content: [{ type: 'text', text: results.length ? `Set FFT: ${results.join(', ')}` : 'No parameters specified' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// Tool: Raw command (escape hatch)
server.tool(
  'mdrop_command',
  'Send a raw IPC command to the visualizer (advanced)',
  {
    command: z.string().describe('Raw pipe command (e.g. "SIGNAL|NEXT_PRESET", "OPACITY=0.5")'),
    expect_response: z.boolean().optional().describe('Whether to wait for a response (default: false)'),
  },
  async ({ command, expect_response }) => {
    try {
      const response = await send(command, expect_response ?? false);
      return { content: [{ type: 'text', text: response || 'Command sent' }] };
    } catch (err) {
      return { content: [{ type: 'text', text: `Error: ${err.message}` }] };
    }
  }
);

// ── Start server ──

const transport = new StdioServerTransport();
await server.connect(transport);
