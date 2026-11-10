"""Validated XML shows, local playback, and Muse reply pagination."""
from collections import OrderedDict
from dataclasses import dataclass
import colorsys
import json
import logging
from pathlib import Path
import queue
import threading
import time

from lxml import etree
from .graphics import frame_commands, render, response_pages, truth
from .palette import strobe_commands

HERE = Path(__file__).resolve().parent
SCHEMA = etree.XMLSchema(etree.parse(str(HERE / "show.xsd")))
log = logging.getLogger(__name__)


@dataclass(frozen=True)
class Scene:
    duration_ms: int
    fade_ms: int
    levels: tuple
    frame: bytes | None
    oled: dict
    cycle: str | None = None
    strobe: tuple = ()


@dataclass(frozen=True)
class Program:
    name: str
    repeat: int | None
    end: str
    scenes: tuple
    responses: dict | None

    def summary(self):
        return {"name": self.name, "scene_count": len(self.scenes),
                "cycle_ms": sum(scene.duration_ms for scene in self.scenes),
                "repeat": self.repeat if self.repeat is not None else "forever", "end": self.end}


def compile_show(xml):
    if not isinstance(xml, str) or not 1 <= len(xml.encode("utf-8")) <= 65536:
        raise ValueError("XML must contain 1..65536 UTF-8 bytes")
    if "<!DOCTYPE" in xml.upper() or "<!ENTITY" in xml.upper():
        raise ValueError("XML document types and entities are not supported")
    parser = etree.XMLParser(resolve_entities=False, load_dtd=False, no_network=True)
    try:
        root = etree.fromstring(xml.encode("utf-8"), parser)
        SCHEMA.assertValid(root)
    except (etree.XMLSyntaxError, etree.DocumentInvalid) as exc:
        raise ValueError("Invalid show XML: " + str(exc).splitlines()[0][:350]) from None
    repeat = root.get("repeat", "1")
    scenes, current = [], (0, 0, 0, 0, 0)
    responses = None
    policy = root.find("responses")
    if policy is not None:
        responses = {"enabled": truth(policy.get("enabled"), True),
                     "page_ms": int(policy.get("page-ms", "4000")),
                     "hold_ms": int(policy.get("hold-ms", "10000"))}
    for element in root.findall("scene"):
        duration, fade = int(element.get("duration-ms")), int(element.get("fade-ms", "0"))
        if fade > duration:
            raise ValueError("fade-ms must not exceed duration-ms")
        values = list(current)
        rgb, white, oled = element.find("rgb"), element.find("white"), element.find("oled")
        native = element.find("cycle")
        custom = element.find("strobe")
        cycle = None
        strobe = ()
        if custom is not None:
            if native is not None or rgb is not None or white is not None or fade:
                raise ValueError("A strobe cannot combine with cycle, rgb, white, or fade-ms")
            colors = custom.findall("color")
            strobe = tuple(strobe_commands(
                [color.get("value") for color in colors], int(custom.get("step-us", "100000")),
                int(custom.get("brightness", "70")), white_palette=[
                    {"warm":int(color.get("warm", "20")), "cool":int(color.get("cool", "0"))}
                    for color in colors]))
        if native is not None:
            if rgb is not None or white is not None or fade:
                raise ValueError("A native cycle cannot combine with rgb, white, or fade-ms")
            step = int(native.get("step-us"))
            if step % 50:
                raise ValueError("cycle step-us must be a multiple of 50")
            cycle = f'CYCLE {step} {int(native.get("brightness", "50"))} {int(native.get("white-brightness", "20"))}'
        if rgb is not None:
            if (rgb.get("color") is None) == (rgb.get("hue") is None):
                raise ValueError("RGB requires exactly one of color or hue")
            brightness = int(rgb.get("brightness", "100")) / 100
            if rgb.get("color") is not None:
                if rgb.get("saturation") is not None:
                    raise ValueError("saturation is only valid with hue")
                color = rgb.get("color")
                channels = [int(color[i:i+2], 16)/255 for i in (1, 3, 5)]
            else:
                channels = colorsys.hsv_to_rgb(int(rgb.get("hue"))/360,
                                               int(rgb.get("saturation", "100"))/100, 1)
            values[:3] = [round(c * brightness * 100) for c in channels]
        if white is not None:
            has_temp = white.get("temperature") is not None
            has_mix = white.get("warm") is not None or white.get("cool") is not None
            if has_temp == has_mix:
                raise ValueError("White requires either temperature, or both warm and cool")
            brightness = int(white.get("brightness", "100"))/100
            if has_temp:
                temperature = int(white.get("temperature"))
                values[3:] = [round((100-temperature)*brightness), round(temperature*brightness)]
            else:
                if white.get("warm") is None or white.get("cool") is None:
                    raise ValueError("Both warm and cool are required")
                values[3:] = [round(int(white.get(c))*brightness) for c in ("warm", "cool")]
        frame = render(oled) if oled is not None else None
        settings = {} if oled is None else {
            "on": truth(oled.get("on"), True), "contrast": int(oled.get("contrast", "160")),
            "invert": truth(oled.get("invert")),
        }
        current = tuple(values)
        scenes.append(Scene(duration, fade, current, frame, settings, cycle, strobe))
    if sum(s.duration_ms for s in scenes) > 600000:
        raise ValueError("One show cycle may not exceed ten minutes")
    return Program(root.get("name", "NuMaker show"), None if repeat == "forever" else int(repeat),
                   root.get("end", "hold"), tuple(scenes), responses)


def schema_payload():
    return {
        "version": 1, "xsd": (HERE / "show.xsd").read_text(),
        "example": (HERE / "demo-show.xml").read_text(),
        "custom_strobe_example": (HERE / "red-yellow-fast.xml").read_text(),
        "limits": {"xml_bytes": 65536, "scenes": 64, "cycle_ms": 600000,
                   "scene_ms": [1000, 60000], "graphics_per_scene": 256},
        "hardware": {
            "oled": {"width": 128, "height": 64, "colors": ["black", "white"],
                     "contrast": [0, 255], "muse_replies": "automatic wrapped, paginated text"},
            "rgb_light": {"color": "#RRGGBB or hue 0..360 + saturation 0..100",
                          "brightness": [0, 100], "channel_resolution_percent": 1},
            "white_light": {"channels": ["warm", "cool"], "brightness": [0, 100],
                            "temperature": "0=warm, 100=cool; uncalibrated mix"},
        },
        "semantics": "Scenes inherit omitted light channels (initially off). Omitted OLED keeps its previous frame. "
                     "For user-chosen fast colors, use <strobe step-us=\"100000\" brightness=\"70\"><color value=\"red\"/><color value=\"yellow\"/></strobe>. Include off for dark RGB steps. Each color can set warm/cool white percentages. "
                     "Use <cycle step-us=\"50\" brightness=\"50\" white-brightness=\"20\"/> instead of rgb/white for native high-speed cycling; step-us is a multiple of 50, at least 50. A later ordinary scene cancels native cycling. "
                     "A new show replaces the previous one only after complete validation. "
                     "Completed Muse replies temporarily override OLED graphics while lights continue. "
                     "Native fades are smooth; scene changes and OLED updates are subject to USB transfer latency.",
    }


class ReplyAssembler:
    def __init__(self):
        self.buffers = OrderedDict()
        self.last = None

    def feed(self, event):
        kind, mid = event.get("event", ""), event.get("message_id") or "current"
        text = event.get("text", "")
        if not isinstance(text, str):
            return None
        if kind == "delta.text_append":
            self.buffers[mid] = (self.buffers.get(mid, "") + text)[-8192:]
            self.buffers.move_to_end(mid)
            while len(self.buffers) > 16:
                self.buffers.popitem(last=False)
            return None
        if kind not in ("message.assistant", "delta.message_done", "delta.presentation"):
            return None
        value = (text or self.buffers.get(mid, ""))[:8192].strip()
        if not value or self.last == (mid, value):
            return None
        self.last = (mid, value)
        self.buffers.pop(mid, None)
        return value


class ShowController:
    """One worker serializes scene transitions and reply pages with direct control."""
    def __init__(self, transport, preferences=None, start_worker=True):
        self.transport = transport
        self.lock = threading.RLock()
        self.wake = threading.Event()
        self.closed = False
        self.program = None
        self.running = False
        self.scene_index = -1
        self.iteration = 0
        self.deadline = 0
        self.last_oled = None
        self.error = ""
        self.render_ms = 0
        self.preferences = preferences
        self.policy = {"enabled": True, "page_ms": 4000, "hold_ms": 10000}
        if preferences and preferences.exists():
            try:
                self.configure(json.loads(preferences.read_text()), save=False)
            except (OSError, ValueError, TypeError):
                pass
        self.events = queue.Queue(maxsize=64)
        self.assembler = ReplyAssembler()
        self.pages = []
        self.page = -1
        self.reply_active = False
        self.reply_deadline = 0
        self.last_reply_at = None
        self.subscribed = False
        self.reply_events_received = 0
        self._thread = None
        if start_worker:
            self._thread = threading.Thread(target=self._worker, name="numaker-show", daemon=True)
            self._thread.start()

    def configure(self, params, save=True):
        if not isinstance(params, dict) or params.keys() - {"enabled", "page_ms", "hold_ms"}:
            raise ValueError("Invalid reply display settings")
        value = {**self.policy, **params}
        if type(value["enabled"]) is not bool:
            raise ValueError("enabled must be a boolean")
        for key in ("page_ms", "hold_ms"):
            if type(value[key]) is not int or not 1000 <= value[key] <= 60000:
                raise ValueError(f"{key} must be an integer from 1000 to 60000")
        self.policy = value
        if not value["enabled"]:
            self.reply_active = False
            self.pages = []
        if save and self.preferences:
            temporary = self.preferences.with_suffix(".tmp")
            temporary.write_text(json.dumps(value))
            temporary.chmod(0o600)
            temporary.replace(self.preferences)
        self.wake.set()
        return self.response_status()

    def play(self, program):
        with self.lock:
            if program.responses is not None:
                self.configure(program.responses)
            self.program = program
            self.running = True
            self.scene_index = -1
            self.iteration = 1
            self.deadline = 0
            self.error = ""
            self.wake.set()
            return self.status()

    def stop(self, off=True):
        with self.lock:
            self.running = False
            self.wake.set()
            if off:
                self.transport.send(["OFF"])
            return self.status()

    def status(self):
        return {"running": self.running, "show": self.program.summary() if self.program else None,
                "scene": self.scene_index + 1 if self.scene_index >= 0 else None,
                "iteration": self.iteration, "last_render_ms": self.render_ms,
                "last_error": self.error or None, "responses": self.response_status()}

    def response_status(self):
        return {**self.policy, "displaying": getattr(self, "reply_active", False),
                "page": getattr(self, "page", -1) + 1,
                "pages": len(getattr(self, "pages", [])),
                "subscribed": getattr(self, "subscribed", False),
                "events_received": getattr(self, "reply_events_received", 0),
                "last_reply_at": getattr(self, "last_reply_at", None)}

    def feed_event(self, event):
        self.reply_events_received += 1
        try:
            self.events.put_nowait(event)
        except queue.Full:
            try:
                self.events.get_nowait()
            except queue.Empty:
                pass
            try:
                self.events.put_nowait(event)
            except queue.Full:
                pass
        self.wake.set()

    def _events(self, now):
        for _ in range(64):
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                break
            value = self.assembler.feed(event)
            if value and self.policy["enabled"]:
                self.pages = response_pages(value)
                self.page = -1
                self.reply_active = True
                self.reply_deadline = 0
                self.last_reply_at = time.time()
                log.info("received Muse reply for OLED (%d pages)", len(self.pages))

    def step(self, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            self._events(now)
            if self.running and now >= self.deadline:
                index = self.scene_index + 1
                if index == len(self.program.scenes):
                    if self.program.repeat is not None and self.iteration >= self.program.repeat:
                        self.running = False
                        if self.program.end == "off":
                            self.transport.send(["OFF"])
                        return
                    self.iteration += 1
                    index = 0
                scene = self.program.scenes[index]
                commands = list(scene.strobe) or [scene.cycle or ("FADE " + str(scene.fade_ms) + " " + " ".join(map(str, scene.levels)))]
                if scene.frame is not None:
                    self.last_oled = scene
                    if not self.reply_active:
                        commands.extend(frame_commands(scene.frame, **scene.oled))
                started = time.monotonic()
                self.transport.send(commands)
                self.render_ms = round((time.monotonic() - started)*1000)
                self.scene_index = index
                self.deadline = now + scene.duration_ms/1000
            if self.reply_active and now >= self.reply_deadline:
                if self.page + 1 < len(self.pages):
                    self.page += 1
                    self.transport.send(frame_commands(self.pages[self.page]))
                    interval = self.policy["page_ms"] if self.page + 1 < len(self.pages) else self.policy["hold_ms"]
                    self.reply_deadline = time.monotonic() + interval/1000
                else:
                    self.reply_active = False
                    if self.running and self.last_oled is not None:
                        self.transport.send(frame_commands(self.last_oled.frame, **self.last_oled.oled))

    def _worker(self):
        while not self.closed:
            self.wake.clear()
            try:
                self.step()
            except Exception as exc:
                with self.lock:
                    self.error = str(exc)[:250]
                    self.running = False
                    self.reply_active = False
                log.warning("show/reply display stopped: %s", self.error)
            self.wake.wait(0.1)

    def close(self):
        self.closed = True
        self.wake.set()
        if self._thread:
            self._thread.join(timeout=2)
