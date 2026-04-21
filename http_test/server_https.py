from pathlib import Path
import os
import ssl

from flask import send_file
from server import app

BASE_DIR = Path(__file__).resolve().parent
CERT_FILE = BASE_DIR / "server.crt"
KEY_FILE = BASE_DIR / "server.key"
LEZ_GB_PATH = BASE_DIR / "lez.gb"


@app.route("/lez.gb")
@app.route("/download/lez.gb")
def download_lez_gb_https():
    return send_file(LEZ_GB_PATH, as_attachment=True, download_name="lez.gb")


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "443"))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    except AttributeError:
        pass
    try:
        ctx.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384")
    except ssl.SSLError:
        pass
    ctx.load_cert_chain(certfile=str(CERT_FILE), keyfile=str(KEY_FILE))
    app.run(host="0.0.0.0", port=port, ssl_context=ctx)
