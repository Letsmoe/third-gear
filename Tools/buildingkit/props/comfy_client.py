"""Minimal ComfyUI HTTP API client: queue a workflow, wait for it, download its outputs."""

import json
import time
import urllib.parse
import urllib.request
import uuid

SERVER = "http://127.0.0.1:8189"


def _request(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(SERVER + path, data=data, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def run_workflow(workflow, timeout_seconds=1800):
    """Queues an API-format workflow and returns the finished history entry; raises on a node error."""
    reply = json.loads(_request("/prompt", {"prompt": workflow, "client_id": str(uuid.uuid4())}))
    prompt_id = reply["prompt_id"]
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        history = json.loads(_request("/history/" + prompt_id))
        if prompt_id in history:
            entry = history[prompt_id]
            status = entry.get("status", {})
            if status.get("status_str") == "error":
                raise RuntimeError(json.dumps(status.get("messages", [])[-3:], indent=1)[:3000])
            return entry
        time.sleep(1.5)
    raise TimeoutError("workflow did not finish")


def download_output(file_info, destination):
    """Saves one output file record (filename, subfolder, type) from the history to a local path."""
    query = urllib.parse.urlencode({"filename": file_info["filename"], "subfolder": file_info.get("subfolder", ""),
                                    "type": file_info.get("type", "output")})
    with open(destination, "wb") as handle:
        handle.write(_request("/view?" + query))


def upload_image(local_path, name):
    """Uploads an image into the input folder so LoadImage can use it."""
    boundary = uuid.uuid4().hex
    with open(local_path, "rb") as handle:
        content = handle.read()
    body = (
        ("--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"%s\"\r\nContent-Type: image/png\r\n\r\n" % (boundary, name)).encode()
        + content + ("\r\n--%s\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--%s--\r\n" % (boundary, boundary)).encode()
    )
    request = urllib.request.Request(SERVER + "/upload/image", data=body,
                                     headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.loads(response.read())["name"]
