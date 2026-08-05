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

ARRAY_SCHEMA = {
    "deviceCtxType": "sv::DeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "feature_ids", "as": "feat_ids", "type": "int", "fmt": "%d",
             "array": {"count": 8, "sep": "-"}},
            {"block": "boxes", "array": {"subRecipe": "box", "count": 4, "sep": "|"}}
        ]}
    ]
}

def test_emits_scalar_array_loop_and_static_assert():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("feat_ids"' in out
    assert 'for (std::size_t i = 0; i < 8; ++i)' in out
    assert 'if (i) out += "-";' in out
    assert 'std::extent_v<decltype(s->feature_ids)> >= 8' in out
    assert '(int)s->feature_ids[i]' in out

def test_emits_struct_array_register_struct_array_and_static_assert():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerStructArray("boxes"' in out
    assert 'std::extent_v<decltype(s->boxes)> >= 4' in out
    assert 'return &s->boxes[i];' in out
    assert '"box", 4, "|"' in out


def run_gen_expect_error(schema_text, needle):
    """Run codegen expecting it to FAIL; assert it mentions `needle` in stderr."""
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(schema_text); schema_path = f.name
    r = subprocess.run([sys.executable, str(GEN), schema_path, tempfile.mktemp() + ".cpp"],
                       capture_output=True, text=True)
    assert r.returncode != 0, f"expected failure but codegen succeeded:\n{r.stdout}"
    assert needle in r.stderr, f"expected stderr to mention {needle!r}; got:\n{r.stderr}"


def test_rejects_array_member_without_field_or_block():
    # M-5: a member with "array" but neither "field" nor "block" is malformed
    # (e.g. a typo'd "block" -> "bloc"). Must fail loudly, not silently skip.
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"bloc": "x", "array": {"count": 2, "sep": "-"}}]}
    ]})
    run_gen_expect_error(schema, "array")


def test_rejects_member_with_no_recognized_key():
    # A member that is neither field, block, nor array — silent skip is wrong.
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"unknown": "thing"}]}
    ]})
    run_gen_expect_error(schema, "Ev")


def test_rejects_scalar_array_missing_count():
    # A scalar array member whose "array" object lacks "count" — must fail
    # with a clear message, not a cryptic KeyError.
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [
            {"field": "ids", "type": "int", "fmt": "%d", "array": {"sep": "-"}}
        ]}
    ]})
    run_gen_expect_error(schema, "count")

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
