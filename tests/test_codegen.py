import json, subprocess, sys, os, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
GEN = ROOT / "tools" / "gen_registry.py"

SCHEMA = {
    "deviceCtxType": "MyDeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "timestamp", "as": "time", "desc": "时间戳", "type": "uint64", "fmt": "%llu"},
            {"block": "person", "desc": "人体", "ptr": True, "sep": "-", "fields": ["name", "age"]},
            {"block": "rect", "desc": "框", "ptr": False, "sep": ",", "fields": ["x"]}
        ]},
        {"name": "PersonInfo", "members": [
            {"field": "name", "desc": "姓名", "type": "cstr", "fmt": "%s"},
            {"field": "age", "desc": "年龄", "type": "int", "fmt": "%d"}
        ]},
        {"name": "Box", "members": [
            {"field": "x", "desc": "X", "type": "int", "fmt": "%d"}
        ]}
    ],
    "device": [{"getter": "cameraId", "as": "camera", "desc": "摄像头"}]
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
    assert '{"name", "age"}' in out   # fields list
    assert '"-"' in out               # sep

def test_emits_device_getter_with_downcast():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerProvider("camera"' in out
    assert 'static_cast<const MyDeviceCtx&>(base)' in out
    assert 'ctx.cameraId()' in out

def test_emits_desc_in_register_calls():
    out = run_gen(json.dumps(SCHEMA), tempfile.mktemp() + ".cpp")
    assert '"时间戳"' in out    # field desc
    assert '"人体"' in out      # block desc
    assert '"摄像头"' in out    # device desc

ARRAY_SCHEMA = {
    "deviceCtxType": "sv::DeviceCtx",
    "structs": [
        {"name": "Event", "members": [
            {"field": "feature_ids", "as": "feat_ids", "type": "int", "fmt": "%d",
             "array": {"count": 8, "sep": "-"}},
            {"block": "boxes", "sep": ",", "fields": ["x", "y"],
             "array": {"count": 4, "sep": "|"}}
        ]}
    ]
}

def test_emits_scalar_array_via_register_scalar_array():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerScalarArray("feat_ids"' in out
    assert 'std::extent_v<decltype(s->feature_ids)> >= 8' in out
    assert '(int)s->feature_ids[i]' in out
    # sep NOT baked into a loop — it's a runtime arg to ScalarArrayProvider
    assert 'if (i) out += "-"' not in out

def test_emits_struct_array_register_struct_array_and_static_assert():
    out = run_gen(json.dumps(ARRAY_SCHEMA), tempfile.mktemp() + ".cpp")
    assert 'registerStructArray("boxes"' in out
    assert 'std::extent_v<decltype(s->boxes)> >= 4' in out
    assert 'return &s->boxes[i];' in out
    assert '{"x", "y"}' in out      # fields
    assert '4' in out               # count
    assert '"|"' in out             # arraySep


def run_gen_expect_error(schema_text, needle):
    """Run codegen expecting it to FAIL; assert it mentions `needle` in stderr."""
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        f.write(schema_text); schema_path = f.name
    r = subprocess.run([sys.executable, str(GEN), schema_path, tempfile.mktemp() + ".cpp"],
                       capture_output=True, text=True)
    assert r.returncode != 0, f"expected failure but codegen succeeded:\n{r.stdout}"
    assert needle in r.stderr, f"expected stderr to mention {needle!r}; got:\n{r.stderr}"


def test_rejects_array_member_without_field_or_block():
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"bloc": "x", "array": {"count": 2, "sep": "-"}}]}
    ]})
    run_gen_expect_error(schema, "array")


def test_rejects_member_with_no_recognized_key():
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"unknown": "thing"}]}
    ]})
    run_gen_expect_error(schema, "Ev")


def test_rejects_scalar_array_missing_count():
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [
            {"field": "ids", "type": "int", "fmt": "%d", "array": {"sep": "-"}}
        ]}
    ]})
    run_gen_expect_error(schema, "count")


def test_rejects_field_name_starting_with_r_():
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"field": "r_x", "type": "int", "fmt": "%d"}]}
    ]})
    run_gen_expect_error(schema, "r_")


def test_rejects_block_name_starting_with_r_():
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"block": "r_bad", "sep": ",", "fields": ["x"]}]}
    ]})
    run_gen_expect_error(schema, "r_")


def test_rejects_block_missing_fields():
    schema = json.dumps({"deviceCtxType": "sv::DeviceCtx", "structs": [
        {"name": "Ev", "members": [{"block": "b", "sep": ",", "ptr": False}]}
    ]})
    run_gen_expect_error(schema, "fields")


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
