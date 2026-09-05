"""Build an isolated panel profile and observe actual loaded plugin modules."""
import configparser
from pathlib import Path
import xml.etree.ElementTree as ET


def inventory():
    result = []
    for path in sorted(Path('/usr/share/xfce4/panel/plugins').glob('*.desktop')):
        entry = configparser.ConfigParser(interpolation=None, strict=False)
        entry.read(path)
        values = entry['Xfce Panel'] if entry.has_section('Xfce Panel') else entry['Desktop Entry']
        result.append({'id': path.stem, 'module': values.get('X-XFCE-Module', ''),
                       'api': values.get('X-XFCE-API', ''), 'metadata': str(path)})
    return result


def configure(directory, plugins):
    root = ET.Element('channel', name='xfce4-panel', version='1.0')
    ET.SubElement(root, 'property', name='configver', type='int', value='2')
    panels = ET.SubElement(root, 'property', name='panels', type='array')
    all_plugins = ET.SubElement(root, 'property', name='plugins', type='empty')
    for group in range((len(plugins) + 9) // 10):
        ET.SubElement(panels, 'value', type='int', value=str(group + 1))
    for group in range((len(plugins) + 9) // 10):
        panel = ET.SubElement(panels, 'property', name=f'panel-{group + 1}', type='empty')
        for name, kind, value in [('position', 'string', f'p=0;x=640;y={40 + group * 80}'),
                                  ('size', 'uint', '32'), ('length', 'uint', '100'),
                                  ('position-locked', 'bool', 'true')]:
            ET.SubElement(panel, 'property', name=name, type=kind, value=value)
        ids = ET.SubElement(panel, 'property', name='plugin-ids', type='array')
        for index in range(group * 10, min(len(plugins), (group + 1) * 10)):
            ET.SubElement(ids, 'value', type='int', value=str(index + 1))
            ET.SubElement(all_plugins, 'property', name=f'plugin-{index + 1}',
                          type='string', value=plugins[index]['id'])
    path = directory / 'xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml'
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(root).write(path, encoding='utf-8', xml_declaration=True)


def loaded_modules(marker):
    loaded = set()
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if b'RILL_TEST_READY_FILE=' + str(marker).encode() + b'\0' not in (process / 'environ').read_bytes():
                continue
            for line in (process / 'maps').read_text().splitlines():
                parts = line.split()
                if parts and parts[-1].endswith('.so'):
                    loaded.add(Path(parts[-1]).name)
        except OSError:
            pass
    return loaded
