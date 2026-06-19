import requests
import asyncio
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from typing import Optional
import traceback
from googletrans import Translator

_cfg = {}
OPENAI_API_URL = None
API_KEY = None
DEFAULT_MODEL = None
TARGET_LANG = "en"
TRANSLATION_TIMEOUT = 10
_executor = ThreadPoolExecutor(max_workers=4)
_services = []


def configure(cfg: dict):
    global _cfg, OPENAI_API_URL, API_KEY, DEFAULT_MODEL, TARGET_LANG, TRANSLATION_TIMEOUT, _services
    _cfg = cfg
    TARGET_LANG = cfg.get("TARGET_LANG", TARGET_LANG)
    TRANSLATION_TIMEOUT = cfg.get("TRANSLATION_TIMEOUT", TRANSLATION_TIMEOUT)

    services = []
    if cfg == {} or cfg.get("google").get("enable"):
        services.append({"name": "google", "timeout": cfg.get("timeout", 5)})
    if cfg.get("openai") and cfg.get("openai").get("enable"):
        OPENAI_API_URL = cfg.get("openai").get("api_url")
        API_KEY = cfg.get("openai").get("api_key")
        DEFAULT_MODEL = cfg.get("openai").get("model", "gpt-4.1-nano")
        services.append({"name": "openai", "timeout": cfg.get("timeout", 8)})
    if cfg.get("external") and cfg.get("external").get("enable"):
        svc = cfg.get("external")
        services.append(
            {"name": "external", "url": svc["url"], "timeout": svc.get("timeout", 6)}
        )

    services.sort(key=lambda s: 0 if s.get("name") == "google" else 1)
    _services = services


def _openai_translate(text: str, timeout: int) -> Optional[str]:
    if not OPENAI_API_URL or not API_KEY:
        print("Error: OPENAI_API_URL or API_KEY is not set.")
        return None

    headers = {
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json",
    }
    system_prompt = "You are a professional translator. Detect the input language automatically and translate the text accurately."
    user_content = (
        f"Please translate the following text to {TARGET_LANG}. "
        "Only return the translated text (do not add explanations):\n\n"
        f"{text}"
    )
    payload = {
        "model": DEFAULT_MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_content},
        ],
        "temperature": 0.0,
        "max_tokens": 2000,
    }

    try:
        resp = requests.post(
            OPENAI_API_URL, headers=headers, json=payload, timeout=timeout
        )
        resp.raise_for_status()
    except requests.RequestException as e:
        print("Request/HTTP error when calling OpenAI API:", e)
        try:
            resp_obj = getattr(e, "response", None)
            if resp_obj is not None:
                print("Response status:", resp_obj.status_code)
                print("Response body:", resp_obj.text)
        except Exception:
            pass
        traceback.print_exc()
        return None

    try:
        data = resp.json()
    except ValueError as e:
        print("Failed to decode JSON response:", e)
        print("Raw response text:", resp.text)
        traceback.print_exc()
        return None

    try:
        translated = None
        if isinstance(data, dict):
            if "choices" in data and data["choices"]:
                first = data["choices"][0]
                msg = first.get("message") or first.get("text")
                if isinstance(msg, dict):
                    translated = msg.get("content")
                elif isinstance(msg, str):
                    translated = msg
            elif "translatedText" in data:
                translated = data.get("translatedText")

        if translated is None:
            print(
                "Warning: couldn't find 'choices' or 'translatedText' in JSON; falling back to raw response text."
            )
            translated = resp.text

        if translated:
            return translated.strip()
    except Exception as e:
        print("Error extracting translation from response:", e)
        print("Response JSON:", data)
        traceback.print_exc()
        return None

    return None


async def _async_translate(text, dest):
    async with Translator() as translator:
        result = await translator.translate(text, dest=dest)
        return getattr(result, "text", str(result))


def _google_translate(text: str, timeout: int) -> Optional[str]:
    def worker():
        try:
            try:
                return asyncio.run(_async_translate(text, TARGET_LANG))
            except RuntimeError:
                loop = asyncio.new_event_loop()
                try:
                    asyncio.set_event_loop(loop)
                    return loop.run_until_complete(_async_translate(text, TARGET_LANG))
                finally:
                    loop.close()
        except Exception:
            try:
                from googletrans import Translator as SyncTranslator

                sync_trans = SyncTranslator()
                res = sync_trans.translate(text, dest=TARGET_LANG)
                return getattr(res, "text", str(res))
            except Exception:
                return None

    future = _executor.submit(worker)
    try:
        return future.result(timeout=timeout)
    except Exception:
        return None


def _external_translate(text: str, url: str, timeout: int) -> Optional[str]:
    payload = {"text": text, "target": TARGET_LANG}
    try:
        resp = requests.post(url, json=payload, timeout=timeout)
        resp.raise_for_status()
        try:
            data = resp.json()
            if isinstance(data, dict):
                for k in ("translated", "translatedText", "translation", "result"):
                    if k in data:
                        return data[k].strip()
                if "choices" in data and data["choices"]:
                    c = data["choices"][0]
                    if isinstance(c, dict):
                        return (
                            c.get("text", "").strip()
                            or c.get("message", {}).get("content", "").strip()
                        )
            return resp.text.strip()
        except Exception:
            return resp.text.strip()
    except Exception:
        return None


def translate_text(text: str, system_prompt: Optional[str] = None) -> str:
    services = globals().get("_services", [])
    if not services:
        return text
    for svc in services:
        name = svc.get("name")
        timeout = svc.get("timeout", 5)
        try:
            if name == "google":
                future = _executor.submit(_google_translate, text, timeout)
            elif name == "openai":
                future = _executor.submit(_openai_translate, text, timeout)
            elif name == "external":
                url = svc.get("url")
                future = _executor.submit(_external_translate, text, url, timeout)
            else:
                continue
            result = future.result(timeout=timeout + 1)
            if result:
                return result
        except TimeoutError:
            print(f"translate timeout: {text}")
            continue
        except Exception:
            continue
    return text
