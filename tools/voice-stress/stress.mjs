// Stress test for the OBS plugin's voice loop (src/voice.cpp, worker_main).
//
// Drives the real sherpa-onnx library through a line-for-line copy of the
// plugin's loop: 48 kHz mono chunks the size OBS hands over, the same linear
// resampler down to 16 kHz, a carry buffer cut into 512-sample windows, the
// Silero VAD with the plugin's exact settings and 30 s buffer, segments copied
// out, popped and decoded with the Moonshine model. Keep it in step with
// worker_main when that changes.
//
// It uses sherpa-onnx's Node package, which calls the same C API the plugin
// does. --runtime swaps in the DLLs a plugin build actually ships, so the run
// tests those binaries rather than npm's.
//
//   npm install
//   powershell -File tts.ps1 -Out wav
//   node --expose-gc stress.mjs --hours 40 [--loop old|new] [--no-babble]
//        [--runtime <data/voice/bin>] [--voice <data/voice>]
//        [--decode-every 1] [--log file]
//
// --loop old is the loop as shipped in 0.7.0; --loop new (the default) is the
// current one. 40 hours of audio takes about 20 minutes. On sherpa-onnx 1.13.8
// with --loop old it crashes near 37.3 hours, which is the bug this guards.

import { createRequire } from "node:module";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));

const args = Object.fromEntries(
  process.argv.slice(2).reduce((pairs, value, index, all) => {
    if (value.startsWith("--")) {
      const next = all[index + 1];
      pairs.push([value.slice(2), next === undefined || next.startsWith("--") ? "true" : next]);
    }
    return pairs;
  }, []),
);

const sherpaDir = path.resolve(args.sherpa ?? here);
const loop = args.loop ?? "new";
const hours = Number(args.hours ?? 1);
const decodeEvery = Number(args["decode-every"] ?? 1);
const logFile = args.log ?? null;
const voiceDir = args.voice ?? "C:/ProgramData/obs-studio/plugins/plasmastream-multistream/data/voice";

const require = createRequire(path.join(sherpaDir, "index.js"));

if (args.runtime) {
  // The addon loads sherpa-onnx-c-api.dll and ONNX Runtime from its own folder.
  const addonDir = path.dirname(require.resolve("sherpa-onnx-win-x64/package.json"));
  for (const dll of ["sherpa-onnx-c-api.dll", "onnxruntime.dll", "onnxruntime_providers_shared.dll"]) {
    fs.copyFileSync(path.join(args.runtime, dll), path.join(addonDir, dll));
  }
}

const sherpa = require("sherpa-onnx-node");

const log = (line) => {
  const text = `[${new Date().toISOString().slice(11, 19)}] ${line}`;
  console.log(text);
  if (logFile) fs.appendFileSync(logFile, text + "\n");
};

// ---- The plugin's constants -------------------------------------------------

const kModelRate = 16000;
const kVadWindow = 512;
const kMaxPhraseSamples = kModelRate * 20;
const kVadFreshSamples = kModelRate * 60 * 60 * 6;
const kObsRate = 48000;

// ---- Engine, as create_engine builds it -------------------------------------

const vad = new sherpa.Vad(
  {
    sileroVad: {
      model: `${voiceDir}/silero_vad.onnx`,
      threshold: 0.5,
      minSilenceDuration: 0.5,
      minSpeechDuration: 0.25,
      windowSize: kVadWindow,
      maxSpeechDuration: 12.0,
    },
    sampleRate: kModelRate,
    numThreads: 1,
    provider: "cpu",
    debug: 0,
  },
  30.0,
);

const recognizer = new sherpa.OfflineRecognizer({
  featConfig: { sampleRate: kModelRate, featureDim: 80 },
  modelConfig: {
    moonshine: {
      encoder: `${voiceDir}/models/fast/encoder_model.ort`,
      mergedDecoder: `${voiceDir}/models/fast/decoder_model_merged.ort`,
    },
    tokens: `${voiceDir}/models/fast/tokens.txt`,
    numThreads: 2,
    provider: "cpu",
    debug: 0,
  },
  decodingMethod: "greedy_search",
});

const resampler = new sherpa.LinearResampler(kObsRate, kModelRate);

function decode(samples) {
  const stream = recognizer.createStream();
  stream.acceptWaveform({ sampleRate: kModelRate, samples });
  recognizer.decode(stream);
  return (recognizer.getResult(stream)?.text ?? "").trim();
}

// ---- Audio --------------------------------------------------------------------
//
// A bank of 20 s clips at 48 kHz, built once and tiled, because synthesizing 40
// hours sample by sample would take longer than the test. Seeded, so a run can
// be repeated exactly.

let seed = 0x5eed1234;
const random = () => {
  seed = (seed * 1664525 + 1013904223) >>> 0;
  return seed / 4294967296;
};
const gauss = () => Math.sqrt(-2 * Math.log(random() + 1e-12)) * Math.cos(2 * Math.PI * random());

const CLIP = kObsRate * 20;

function speechLike(withPauses, level) {
  const out = new Float32Array(CLIP);
  let phase = 0;
  let f0 = 120 + random() * 100;
  let gate = 1;
  let gateLeft = kObsRate * (1 + random() * 3);
  for (let i = 0; i < CLIP; i++) {
    if (--gateLeft <= 0) {
      gate = withPauses ? (gate ? 0 : 1) : 1;
      gateLeft = kObsRate * (gate ? 1 + random() * 3 : 0.3 + random() * 0.9);
    }
    if (i % 480 === 0) f0 = Math.min(240, Math.max(90, f0 + gauss() * 4));
    phase += (2 * Math.PI * f0) / kObsRate;
    const t = i / kObsRate;
    const syllable = Math.abs(Math.sin(2 * Math.PI * 4 * t + Math.sin(t)));
    let voiced = 0;
    for (let h = 1; h <= 12; h++) {
      const freq = f0 * h;
      const formant = Math.exp(-((freq - 600) ** 2) / 90000) + 0.6 * Math.exp(-((freq - 1700) ** 2) / 200000) + 0.2;
      voiced += (formant * Math.sin(phase * h)) / h;
    }
    out[i] = level * gate * syllable * (voiced + 0.05 * gauss());
  }
  return out;
}

function noise(level, tones) {
  const out = new Float32Array(CLIP);
  let b0 = 0, b1 = 0, b2 = 0;
  for (let i = 0; i < CLIP; i++) {
    const white = gauss();
    b0 = 0.997 * b0 + 0.029591 * white;
    b1 = 0.985 * b1 + 0.032534 * white;
    b2 = 0.95 * b2 + 0.048056 * white;
    const t = i / kObsRate;
    const music = tones ? 0.3 * Math.sin(2 * Math.PI * 220 * t) * Math.sin(2 * Math.PI * 0.5 * t) + 0.2 * Math.sin(2 * Math.PI * 330 * t) : 0;
    out[i] = level * (b0 + b1 + b2 + 0.1 * white + music);
  }
  return out;
}

const mix = (a, b) => a.map((value, i) => value + b[i % b.length]);

// Real speech, rendered by the Windows speech synthesizer at 48 kHz (tts.ps1):
// short commands with gaps, and a long run-on monologue with no real pause.
function wav(file) {
  const bytes = fs.readFileSync(file);
  let offset = 12;
  while (offset < bytes.length) {
    const id = bytes.toString("ascii", offset, offset + 4);
    const size = bytes.readUInt32LE(offset + 4);
    if (id === "data") {
      const out = new Float32Array(size / 2);
      for (let i = 0; i < out.length; i++) out[i] = bytes.readInt16LE(offset + 8 + i * 2) / 32768;
      return out;
    }
    offset += 8 + size;
  }
  throw new Error(`no audio in ${file}`);
}

const wavDir = path.join(here, "wav");
const talk = fs.readdirSync(wavDir).filter((f) => f.startsWith("talk")).map((f) => wav(path.join(wavDir, f)));
const monologue = fs.readdirSync(wavDir).filter((f) => f.startsWith("monologue")).map((f) => wav(path.join(wavDir, f)));

const bank = {
  quiet: [noise(0.002, false), noise(0.004, false)],
  talk: [...talk, speechLike(true, 0.25)],
  monologue,
  game: [noise(0.15, true), noise(0.3, true)],
  talkOverGame: [...talk.map((t) => mix(t, noise(0.12, true))), ...monologue.map((m) => mix(m, noise(0.2, true)))],
  // Several people at once, as in a party chat coming through the speakers:
  // speech with no gap anywhere, which is what never lets a phrase end.
  babble: [mix(mix(monologue[0], monologue[1].subarray(kObsRate * 7)), talk[0].subarray(kObsRate * 3))],
};
// How the hours are spent, roughly a long stream with the mic open.
const scenes = [
  ["quiet", 0.3],
  ["talk", 0.25],
  ["monologue", 0.1],
  ["game", 0.15],
  ["talkOverGame", 0.15],
  // --no-babble leaves it out, for the old loop, where one babble phrase grows
  // to minutes and several gigabytes before the 37-hour mark it is run to reach.
  ["babble", args["no-babble"] !== undefined ? 0 : 0.05],
];

function pickScene() {
  let roll = random();
  for (const [name, weight] of scenes) {
    if ((roll -= weight) <= 0) return name;
  }
  return "quiet";
}

// ---- The worker loop ----------------------------------------------------------

const totalInput = Math.round(hours * 3600 * kObsRate);
let fed = 0;
let carry = new Float32Array(0);
const window = new Float32Array(kVadWindow);

const stats = { segments: 0, decoded: 0, longestPhrase: 0, flushes: 0, resets: 0, vadSamples: 0, detectedWindows: 0, windows: 0, words: 0 };
let vadSamples = 0;
let phraseSamples = 0;

const started = Date.now();
let nextReport = kObsRate * 3600;
let sceneName = pickScene();
let sceneLeft = kObsRate * (5 + random() * 600);
let clip = bank[sceneName][0];
let clipPos = 0;
const gain = () => 0.6 + random() * 0.8;
let sceneGain = gain();

log(`sherpa ${sherpaDir.split(/[\\/]/).slice(-1)[0]}, loop ${loop}, ${hours} h at 48 kHz, decoding every ${decodeEvery} segment(s)`);

while (fed < totalInput) {
  // OBS hands audio over 1024 frames at a time; the worker wakes at a tenth of a
  // second, so a chunk is 5 to 7 of those.
  const frames = 1024 * (5 + Math.floor(random() * 3));
  const chunk = new Float32Array(frames);

  for (let i = 0; i < frames; i++) {
    if (--sceneLeft <= 0) {
      sceneName = pickScene();
      sceneLeft = kObsRate * (5 + random() * 600);
      const choices = bank[sceneName];
      clip = choices[Math.floor(random() * choices.length)];
      clipPos = Math.floor(random() * clip.length);
      sceneGain = gain();
    }
    chunk[i] = clip[clipPos] * sceneGain;
    clipPos = (clipPos + 1) % clip.length;
  }
  fed += frames;

  const resampled = resampler.resample(chunk);
  if (!resampled || resampled.length === 0) continue;

  const joined = new Float32Array(carry.length + resampled.length);
  joined.set(carry);
  joined.set(resampled, carry.length);
  carry = joined;

  let used = 0;

  while (carry.length - used >= kVadWindow) {
    window.set(carry.subarray(used, used + kVadWindow));
    vad.acceptWaveform(window);
    used += kVadWindow;
    stats.windows++;

    let inPhrase = false;

    if (loop === "new") {
      vadSamples += kVadWindow;
      inPhrase = vad.isDetected();
      phraseSamples = inPhrase ? phraseSamples + kVadWindow : 0;

      if (phraseSamples >= kMaxPhraseSamples) {
        vad.flush();
        phraseSamples = 0;
        stats.flushes++;
      }
    } else if (vad.isDetected()) {
      inPhrase = true;
    }

    if (inPhrase) stats.detectedWindows++;

    while (!vad.isEmpty()) {
      const segment = vad.front(false);
      const phrase = segment && segment.samples ? Float32Array.from(segment.samples) : new Float32Array(0);
      vad.pop();
      stats.segments++;
      stats.longestPhrase = Math.max(stats.longestPhrase, phrase.length / kModelRate);

      if (phrase.length > 0 && stats.segments % decodeEvery === 0) {
        const text = decode(phrase);
        stats.decoded++;
        stats.words += text ? text.split(/\s+/).length : 0;
      }
    }

    if (loop === "new" && vadSamples >= kVadFreshSamples && !inPhrase) {
      vad.reset();
      vadSamples = 0;
      phraseSamples = 0;
      stats.resets++;
    }
  }

  carry = carry.slice(used);

  if (fed >= nextReport) {
    nextReport += kObsRate * 3600;
    // The Node wrapper frees decoder streams and results only when the garbage
    // collector finalizes them, and with a small JS heap that is rarely. Run
    // with --expose-gc so memory reads as the library's, not the wrapper's.
    // Node runs an addon's finalizers on a later turn of the event loop, which
    // this loop otherwise never gives it.
    globalThis.gc?.();
    await new Promise((resolve) => setImmediate(resolve));
    globalThis.gc?.();
    const sim = fed / kObsRate / 3600;
    const counted = stats.windows * kVadWindow;
    log(
      `${sim.toFixed(1)} h fed (${(counted / 2 ** 31 * 100).toFixed(1)}% of 2^31 at 16 kHz), ` +
        `${((Date.now() - started) / 1000).toFixed(0)} s, segments ${stats.segments}, decoded ${stats.decoded}, ` +
        `longest ${stats.longestPhrase.toFixed(1)} s, flushes ${stats.flushes}, resets ${stats.resets}, ` +
        `speech ${((stats.detectedWindows / stats.windows) * 100).toFixed(0)}%, rss ${(process.memoryUsage().rss / 1e6).toFixed(0)} MB`,
    );
  }
}

log(`PASS: ${hours} h without a crash. ${JSON.stringify(stats)}`);
