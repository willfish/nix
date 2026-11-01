import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

const DEFAULT_VOICE_ID = "JBFqnCBsd6RMkjVDRZzb"; // George (British, warm, natural)
const DEFAULT_MODEL_ID = "eleven_v3";

function loadApiKey() {
  if (process.env.ELEVENLABS_API_KEY) return process.env.ELEVENLABS_API_KEY.trim();
  const sopsDir = process.env.SOPS_NIX_SECRETS_DIR
    || join(process.env.XDG_CONFIG_HOME || join(homedir(), ".config"), "sops-nix", "secrets");
  try {
    return readFileSync(join(sopsDir, "ELEVENLABS_API_KEY"), "utf8").trim().replace(/^"|"$/g, "");
  } catch {
    throw new Error("ELEVENLABS_API_KEY is not set and could not be read from sops-nix secrets");
  }
}

function playPcm24k(pcmBuffer) {
  const wav = Buffer.alloc(44 + pcmBuffer.length);
  wav.write("RIFF", 0);
  wav.writeUInt32LE(36 + pcmBuffer.length, 4);
  wav.write("WAVE", 8);
  wav.write("fmt ", 12);
  wav.writeUInt32LE(16, 16);
  wav.writeUInt16LE(1, 20); // PCM
  wav.writeUInt16LE(1, 22); // Mono
  wav.writeUInt32LE(24000, 24); // Sample rate
  wav.writeUInt32LE(48000, 28); // Byte rate
  wav.writeUInt16LE(2, 32); // Block align
  wav.writeUInt16LE(16, 34); // Bits per sample
  wav.write("data", 36);
  wav.writeUInt32LE(pcmBuffer.length, 40);
  pcmBuffer.copy(wav, 44);

  const player = process.platform === "darwin" ? "afplay" : "pw-play";
  const args = process.platform === "darwin" ? ["-"] : ["-"];
  const child = spawn(player, args, { stdio: ["pipe", "ignore", "ignore"], detached: true });
  child.on("error", () => {});
  child.stdin.on("error", () => {});
  child.stdin.end(wav);
  child.unref();
}

export async function setupSpeak(ctx) {
  await ctx.tool.transform(editor => editor.add({
    name: "speak",
    description:
      "Speak short messages aloud to the user using ElevenLabs v3 TTS (non-blocking). " +
      "Supports inline expressive tags such as [excited], [sighs], [laughs], [whispers], [curious], [calmly]. " +
      "Keep spoken messages concise (1-2 sentences) for completions, summaries, or questions.",
    options: { codemode: false },
    input: {
      type: "object",
      properties: {
        text: {
          type: "string",
          minLength: 1,
          maxLength: 2500,
          description: "Text to speak aloud. Can include inline audio tags like [sighs], [excited], [laughs].",
        },
        voice_id: {
          type: "string",
          description: "Optional ElevenLabs voice ID (defaults to George: JBFqnCBsd6RMkjVDRZzb, or ELEVENLABS_VOICE_ID env).",
        },
        model_id: {
          type: "string",
          description: "Optional ElevenLabs model ID (defaults to eleven_v3; can also be eleven_flash_v2_5 or eleven_turbo_v2_5).",
        },
      },
      required: ["text"],
      additionalProperties: false,
    },
    async execute(input, context) {
      await ctx.permission.assert({ action: "speak", resources: ["*"], sessionID: context.sessionID, agent: context.agent });
      const apiKey = loadApiKey();
      const voiceId = input.voice_id || process.env.ELEVENLABS_VOICE_ID || DEFAULT_VOICE_ID;
      const modelId = input.model_id || process.env.ELEVENLABS_MODEL_ID || DEFAULT_MODEL_ID;
      const response = await fetch(
        `https://api.elevenlabs.io/v1/text-to-speech/${encodeURIComponent(voiceId)}?output_format=pcm_24000`,
        {
          method: "POST",
          headers: {
            "xi-api-key": apiKey,
            "Content-Type": "application/json",
          },
          body: JSON.stringify({
            text: input.text,
            model_id: modelId,
          }),
          signal: context.signal,
        }
      );
      if (!response.ok) {
        const errText = await response.text().catch(() => "");
        throw new Error(`ElevenLabs TTS failed (HTTP ${response.status}): ${errText}`);
      }
      const pcm = Buffer.from(await response.arrayBuffer());
      playPcm24k(pcm);
      return { content: JSON.stringify({ ok: true, voice_id: voiceId, model_id: modelId, bytes: pcm.length }) };
    },
  }));
}
