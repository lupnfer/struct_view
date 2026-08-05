import json, subprocess, sys, os, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
GEN = ROOT / "tools" / "gen_registry.py"

SCHEMA = {
    "deviceCtxType": "MyDeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "timestamp", "as": "time", "type": "uint64", "fmt": "%llu"},
            {"block": "person", "ptr": True, "subRecipe": "person"},
            {"block": "rect", "ptr": False, "subRecipe": "rect"}
        ]},
        {"name": "PersonInfo", "members": [
            {"field": "name", "type": "cstr", "fmt": "%s"},
            {"field": "age", "type": "int", "fmt": "%d"}
        ]},
        {"name": "Box", "members": [
            {"field": "x", "type": "int", "fmt": "%d"}
        ]}
    ],
    "device": [{"getter": "cameraId", "as": "camera"}]
}

def run_gen(schema_text, out_path):
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(schema_text); schema_path = f.name
    r = subprocess.run([sys.executable, str(GEN), schema_path, str(out_path)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return pathlib.Path(out_path).read_text()

def test_emits_field_with_typed_cast():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("time"' in out
    assert '(unsigned long long)' in out   # uint64 cast
    assert 's->timestamp' in out

def test_emits_cstr_field():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("name"' in out
    assert '(const char*)' in out
    assert 's->name' in out

def test_emits_struct_block_pointer_and_embedded():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'return static_cast<const void*>(s->person);' in out   # ptr
    assert 'return static_cast<const void*>(&s->rect);' in out    # embedded

def test_emits_device_getter_with_downcast():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("camera"' in out
    assert 'static_cast<const MyDeviceCtx&>(base)' in out
    assert 'ctx.cameraId()' in out

if __name__ == "__main__":
    import traceback
    fns = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    failed = 0
    for fn in fns:
        try:
            fn(); print(f"PASS {fn.__name__}")
        except Exception:
            failed += 1; print(f"FAIL {fn.__name__}"); traceback.print_exc()
    sys.exit(1 if failed else 0)
