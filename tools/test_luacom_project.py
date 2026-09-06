"""Require the native project to include every imported C/C++ source and header."""
from pathlib import PureWindowsPath
import unittest
import xml.etree.ElementTree as ET

from luacom_vendor import ROOT


class ProjectInventoryTests(unittest.TestCase):
    def test_project_lists_every_imported_source_and_header(self):
        project = ET.parse(ROOT / 'MUSHclient.vcxproj')
        namespace = {'msbuild': 'http://schemas.microsoft.com/developer/msbuild/2003'}
        roles = {'ClCompile': {'.c', '.cc', '.cpp', '.cxx'},
                 'ClInclude': {'.h', '.hh', '.hpp', '.hxx'}}
        for role, extensions in roles.items():
            with self.subTest(role=role):
                expected = {'luacom/' + path.name for path in (ROOT / 'luacom').iterdir()
                            if path.suffix.lower() in extensions}
                actual = []
                for item in project.findall('.//msbuild:' + role, namespace):
                    path = PureWindowsPath(item.get('Include', '')).as_posix()
                    if path.startswith('luacom/'):
                        actual.append(path)
                self.assertEqual(len(actual), len(set(actual)), 'Duplicate LuaCOM project entries')
                self.assertEqual(set(actual), expected,
                                 'Update the native project when the upstream source inventory changes')


if __name__ == '__main__':
    unittest.main()
