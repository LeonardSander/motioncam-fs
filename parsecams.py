import xml.etree.ElementTree as ET
from pathlib import Path

root = ET.parse('cameras.xml').getroot()
wanted = ['Canon EOS 5D Mark III','Canon EOS R5','Sony ILCE-7M3','Panasonic DC-S1H','Nikon Z 9']
found = {}
for cam in root.findall('.//Camera'):  # cameras
    name = (cam.get('make','') + ' ' + cam.get('model','')).strip()
    for w in wanted:
        if w.lower() == name.lower():
            found[w] = cam

for w in wanted:
    cam = found.get(w)
    print('\n', w, 'FOUND' if cam is not None else 'MISSING')
    if cam is None:
        continue
    cms = cam.findall('CalibrationMatrices/ColorMatrix')
    fms = cam.findall('ForwardMatrices/ColorMatrix')
    def parse(node):
        return [float(x) for x in node.text.strip().split(',')]
    for idx, cm in enumerate(cms, 1):
        print(' CM', idx, parse(cm))
    for idx, fm in enumerate(fms, 1):
        print(' FM', idx, parse(fm))
