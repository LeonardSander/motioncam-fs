import re, pathlib
text = pathlib.Path('libraw/src/tables/colordata.cpp').read_text()
pattern = r'\{\s*LIBRAW_CAMERAMAKER_([A-Za-z0-9_]+),\s*\"([^\"]+)\",\s*0[^,]*,\s*0[^,]*,\s*\{([0-9,\s-]+)\}\s*\}'
entries = []
for m in re.finditer(pattern, text):
    maker = m.group(1)
    model = m.group(2)
    nums = [int(x.strip()) for x in m.group(3).split(',') if x.strip()]
    if len(nums) == 9:
        entries.append((maker, model, nums))

wanted = [
    ('Canon', 'EOS 5D Mark III'),
    ('Sony', 'ILCE-7M3'),
    ('Panasonic', 'DC-S1H'),
    ('Nikon', 'Z 9'),
    ('Canon', 'EOS R5'),
]
for maker, model in wanted:
    for e in entries:
        if e[0].lower() == maker.lower() and e[1] == model:
            print(f"{maker} {model}: {e[2]}")
            break
