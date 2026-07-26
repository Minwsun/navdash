Import("env")

from hashlib import sha256
from pathlib import Path

expected = "a80028cdf3c88102b0d1aa40ad385a2be5dd2f8aa4ce185e50503bdeff04c19f"
path = Path(env["PROJECT_DIR"]) / "src" / "royal_dash.cpp"
actual = sha256(path.read_bytes()).hexdigest()
if actual != expected:
    raise SystemExit(f"Pairing lock violation: {path} hash {actual}; expected {expected}")