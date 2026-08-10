#!/usr/bin/env python3
"""Generate an initial schema.json from a C header file (.h).

Usage: gen_schema.py input.h output.json
Parses typedef struct { ... } Name; patterns and infers field types,
arrays, struct blocks (pointer/embedded), and struct arrays. Produces
a schema.json with sensible defaults; the user then fills in desc,
sep, fields, and other metadata that can't be inferred from .h alone.

Type inference:
  uint64_t / uint32_t / int64_t etc → "uint64" (if 64-bit) or "int"
  int / int32_t / short / long      → "int"
  float / double                    → "float"
  char[N] / char*                   → "cstr"
  T* (struct pointer)               → block with ptr=true
  T (struct embedded)               → block with ptr=false (default, omitted)
  T name[N] (scalar array)          → field + array {count: N}
  T name[N] (struct array)          → block + array {count: N}

What the user still needs to fill in:
  - desc (Chinese description for client display)
  - block.sep + block.fields (how to expand the struct block)
  - array.sep (default separator)
  - as (friendly name, if different from C member name)
  - deviceCtxType + device getters
"""
import json, re, sys, pathlib

# C type → schema type mapping
def infer_type(c_type):
    """Infer schema type from a C type string (without array brackets)."""
    c_type = c_type.strip()
    # Remove pointer (handled separately for struct blocks)
    if c_type.endswith('*'):
        c_type = c_type[:-1].strip()
    # 64-bit unsigned
    if c_type in ('uint64_t', 'int64_t', 'unsigned long long', 'long long', 'size_t'):
        return 'uint64'
    # Standard int types
    if c_type in ('int', 'int32_t', 'uint32_t', 'short', 'unsigned short',
                  'unsigned int', 'long', 'unsigned long', 'unsigned'):
        return 'int'
    # Float types
    if c_type in ('float', 'double'):
        return 'float'
    # Char types → C string
    if c_type.startswith('char'):
        return 'cstr'
    # Unknown — default to int
    return 'int'

def parse_header(text):
    """Parse typedef struct { ... } Name; from C header text. Returns list of
    (struct_name, [(c_type, field_name, array_size_or_None), ...])."""
    structs = []
    # Match: typedef struct { fields } Name;
    # Fields can span multiple lines; comments stripped first.
    text_clean = re.sub(r'//[^\n]*', '', text)  # strip // comments
    text_clean = re.sub(r'/\*.*?\*/', '', text_clean, flags=re.DOTALL)  # strip /* */ comments
    text_clean = re.sub(r'#[^\n]*', '', text_clean)  # strip preprocessor

    pattern = r'typedef\s+struct\s*\{([^}]*)\}\s*(\w+)\s*;'
    for match in re.finditer(pattern, text_clean, re.DOTALL):
        body = match.group(1).strip()
        name = match.group(2).strip()
        members = []
        # Split on ; to get individual field declarations
        for decl in body.split(';'):
            decl = decl.strip()
            if not decl:
                continue
            # Handle comma-separated fields: "int x, y, w, h"
            # Split type from field list
            parts = decl.split()
            if len(parts) < 2:
                continue
            # Find where type ends and field names begin
            # Type can be multi-word: "unsigned int", "unsigned long long"
            # Heuristic: last token(s) are field names (possibly with array brackets)
            # Find the first field-name-like token (not a known type keyword)
            type_keywords = {'unsigned', 'signed', 'const', 'struct', 'enum', 'volatile'}
            # Collect type tokens until we hit something that looks like a field name
            type_tokens = []
            field_decls = []
            tokens = decl.split()
            i = 0
            while i < len(tokens):
                t = tokens[i]
                # Check if this token is a type keyword or a known C type
                # Also handle '*' that may be attached to the type: "PersonInfo*" or "PersonInfo *"
                if (t in type_keywords or
                    t in ('char', 'short', 'int', 'long', 'float', 'double', 'void') or
                    re.match(r'^[u]?int\d*_t$', t) or
                    re.match(r'^size_t$', t) or
                    re.match(r'^[A-Z]\w*\*?$', t) or  # Struct type name (CamelCase) with optional *
                    t == '*'):
                    type_tokens.append(t)
                    i += 1
                else:
                    break
            # Remaining tokens are field declarations (possibly comma-separated with array brackets)
            c_type = ' '.join(type_tokens)
            field_part = ' '.join(tokens[i:])
            # Split on commas, but handle array brackets
            for fd in field_part.split(','):
                fd = fd.strip()
                if not fd:
                    continue
                # Extract array size if present: name[N]
                arr_match = re.match(r'(\w+)\s*\[(\w+)\]', fd)
                if arr_match:
                    fname = arr_match.group(1)
                    arr_size = arr_match.group(2)
                    # Try to convert to int (if it's a number, not a macro)
                    try:
                        arr_size = int(arr_size)
                    except ValueError:
                        arr_size = None  # macro or unknown size — user fills in
                    members.append((c_type, fname, arr_size))
                else:
                    members.append((c_type, fd, None))
        structs.append((name, members))
    return structs

def is_struct_type(c_type, known_struct_names):
    """Check if a C type is a known struct type (not a primitive)."""
    c_type = c_type.strip()
    # Remove trailing * (with optional space)
    if c_type.endswith('*'):
        c_type = c_type[:-1].strip()
    primitives = {'int', 'short', 'long', 'float', 'double', 'char',
                  'unsigned', 'signed', 'const', 'void', 'volatile', 'struct',
                  'uint64_t', 'int64_t', 'uint32_t', 'int32_t', 'uint16_t',
                  'int16_t', 'uint8_t', 'int8_t', 'size_t', 'ssize_t',
                  'unsigned int', 'unsigned short', 'unsigned long',
                  'unsigned char', 'long long', 'unsigned long long',
                  'signed int', 'signed short', 'signed long', 'signed char'}
    return c_type in known_struct_names and c_type not in primitives

def is_char_array(c_type, arr_size):
    """Check if this is char[N] — should be treated as cstr, not a scalar array."""
    c_type = c_type.strip()
    return c_type.startswith('char') and arr_size is not None

def gen_schema(text):
    """Generate schema dict from C header text."""
    structs = parse_header(text)
    struct_names = {name for name, _ in structs}

    schema = {
        "deviceCtxType": "TODO_FillDeviceCtxType",
        "structs": [],
        "device": []
    }

    for struct_name, members in structs:
        struct_entry = {"name": struct_name, "members": []}
        for c_type, fname, arr_size in members:
            is_ptr = '*' in c_type
            base_type = c_type.replace('*', '').strip()

            if is_struct_type(base_type, struct_names):
                # Struct member → block
                member = {
                    "block": fname,
                    "desc": "",  # user fills in
                    "sep": "",   # user fills in
                    "fields": [] # user fills in (field names of the struct)
                }
                if is_ptr:
                    member["ptr"] = True
                if arr_size is not None:
                    member["array"] = {"count": arr_size, "sep": ""}
                struct_entry["members"].append(member)
            elif is_char_array(base_type, arr_size):
                # char[N] → cstr (C string, not a scalar array)
                member = {
                    "field": fname,
                    "desc": "",  # user fills in
                    "type": "cstr"
                }
                struct_entry["members"].append(member)
            else:
                # Scalar field
                stype = infer_type(base_type)
                member = {
                    "field": fname,
                    "desc": "",  # user fills in
                    "type": stype
                }
                if arr_size is not None:
                    member["array"] = {"count": arr_size, "sep": ""}
                struct_entry["members"].append(member)
        schema["structs"].append(struct_entry)

    return schema

def main():
    if len(sys.argv) != 3:
        print("usage: gen_schema.py input.h output.json", file=sys.stderr)
        sys.exit(2)
    text = pathlib.Path(sys.argv[1]).read_text()
    schema = gen_schema(text)
    pathlib.Path(sys.argv[2]).write_text(json.dumps(schema, indent=2, ensure_ascii=False) + "\n")
    print(f"Generated schema from {sys.argv[1]} → {sys.argv[2]}", file=sys.stderr)
    print("TODO: fill in desc, sep, fields, deviceCtxType, device getters.", file=sys.stderr)

if __name__ == "__main__":
    main()
