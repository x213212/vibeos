from pathlib import Path
from flask import Flask, send_file
import os

app = Flask(__name__)

BASE_DIR = Path(__file__).resolve().parent
FILE_PATH = BASE_DIR / "test.bmp"
BIN_PATH = BASE_DIR / "test.bin"
ELF_PATH = BASE_DIR / "hello.elf"
FAULT_BIN_PATH = BASE_DIR / "fault.bin"
WRITEFAULT_BIN_PATH = BASE_DIR / "writefault.bin"
JUMPFAULT_BIN_PATH = BASE_DIR / "jumpfault.bin"
SELFMEM_BIN_PATH = BASE_DIR / "selfmem.bin"
LEZ_GB_PATH = BASE_DIR / "lez.gb"


@app.route("/")
def index():
    return "OK\n"


@app.route("/test.html")
def download_test_html():
    return send_file(BASE_DIR / "test.html", mimetype="text/html")


@app.route("/test.bmp")
@app.route("/download")
def download_file():
    return send_file(FILE_PATH, as_attachment=True, download_name="test.bmp")


@app.route("/test.bin")
@app.route("/download.bin")
def download_bin():
    return send_file(ELF_PATH, as_attachment=True, download_name="test.bin")


@app.route("/lez.gb")
@app.route("/download/lez.gb")
def download_lez_gb():
    return send_file(LEZ_GB_PATH, as_attachment=True, download_name="lez.gb")


@app.route("/hello.elf")
def download_elf():
    return send_file(ELF_PATH, as_attachment=True, download_name="hello.elf")


@app.route("/fault.bin")
def download_fault_bin():
    return send_file(FAULT_BIN_PATH, as_attachment=True, download_name="fault.bin")


@app.route("/writefault.bin")
def download_writefault_bin():
    return send_file(WRITEFAULT_BIN_PATH, as_attachment=True, download_name="writefault.bin")


@app.route("/jumpfault.bin")
def download_jumpfault_bin():
    return send_file(JUMPFAULT_BIN_PATH, as_attachment=True, download_name="jumpfault.bin")


@app.route("/selfmem.bin")
def download_selfmem_bin():
    return send_file(SELFMEM_BIN_PATH, as_attachment=True, download_name="selfmem.bin")


if __name__ == "__main__":
    port = int(os.environ.get("PORT", "80"))
    app.run(host="0.0.0.0", port=port)
