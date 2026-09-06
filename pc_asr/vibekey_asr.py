"""VibeKey Stage-7 voice-bypass ASR service (Windows sidecar).

Custom (non-vibetty) bypass: a VibeKey C5 captures PDM audio, software-decimates
to 16 kHz mono PCM and streams 20 ms frames on {prefix}/audio_pcm, bracketed by
{prefix}/audio_ctl {"cmd":"start"|"stop"} JSON. This process runs Vosk (offline)
over the stream and, on stop, publishes the recognised text back into vibetty as
{"type":"input_text","data":"<text>"} on {prefix}/control -- so the words land in
the AI agent's terminal without any vibetty protocol change.

Usage:
  vibekey_asr.py [broker] [prefix]
Defaults: broker=broker.emqx.io:1883  prefix=root/abc123/999/vibetty
"""
import json
import sys
import threading

import paho.mqtt.client as mqtt
from vosk import Model, KaldiRecognizer

BROKER = sys.argv[1] if len(sys.argv) > 1 else "broker.emqx.io:1883"
PREFIX = sys.argv[2] if len(sys.argv) > 2 else "root/abc123/999/vibetty"
MODEL_PATH = sys.argv[3] if len(sys.argv) > 3 else "vosk-model-small-en-us-0.15"
SAMPLE_RATE = 16000

CTL_TOPIC = PREFIX + "/audio_ctl"
PCM_TOPIC = PREFIX + "/audio_pcm"
CONTROL_TOPIC = PREFIX + "/control"

_model = Model(MODEL_PATH)
_state = {"rec": None, "buf": b""}
_lock = threading.Lock()


def _recogniser():
    return KaldiRecognizer(_model, SAMPLE_RATE)


def _flush():
    """Finalise current utterance, publish any text to vibetty control."""
    with _lock:
        rec, _buf = _state["rec"], _state["buf"]
        _state["rec"] = None
        _state["buf"] = b""
    if rec is None:
        return
    try:
        text = json.loads(rec.FinalResult()).get("text", "").strip()
    except Exception as e:
        print(f"[asr] final result error: {e}")
        return
    if not text:
        print("[asr] (no speech detected)")
        return
    payload = json.dumps({"type": "input_text", "data": text})
    client.publish(CONTROL_TOPIC, payload, qos=1)
    print(f"[asr] -> control input_text: {text!r}")


def on_connect(c, userdata, flags, rc, properties=None):
    print(f"[asr] connected rc={rc}")
    if rc != 0:
        return
    c.subscribe(CTL_TOPIC, qos=1)
    c.subscribe(PCM_TOPIC, qos=1)
    print(f"[asr] subscribed {CTL_TOPIC} and {PCM_TOPIC}")


def on_message(c, userdata, msg):
    try:
        if msg.topic == CTL_TOPIC:
            cmd = json.loads(msg.payload.decode()).get("cmd")
            if cmd == "start":
                with _lock:
                    _state["rec"] = _recogniser()
                    _state["buf"] = b""
                print("[asr] start")
            elif cmd == "stop":
                print("[asr] stop")
                _flush()
            elif cmd == "reset":
                with _lock:
                    _state["rec"] = _recogniser()
                    _state["buf"] = b""
                print("[asr] reset")
        elif msg.topic == PCM_TOPIC:
            with _lock:
                rec = _state["rec"]
                if rec is not None:
                    _state["buf"] += msg.payload
                pending = _state["buf"] if rec is not None else b""
                _state["buf"] = b"" if rec is not None else _state["buf"]
            if rec is not None and pending:
                rec.AcceptWaveform(pending)
                partial = json.loads(rec.PartialResult()).get("partial", "")
                if partial:
                    print(f"[asr] partial: {partial!r}")
    except Exception as e:
        import traceback
        traceback.print_exc()


def main():
    global client
    host, port = BROKER.split(":")
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                         client_id=f"vibekey-asr-{int(__import__('time').time())}",
                         clean_session=True)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(host, int(port), 30)
    print(f"[asr] running broker={BROKER} prefix={PREFIX}")
    client.loop_forever()


if __name__ == "__main__":
    main()
