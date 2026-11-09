"""Check real temporary dylib chains without executing update or USB functions."""
import subprocess, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import firmware

@unittest.skipUnless(sys.platform=='darwin', 'requires macOS dylibs')
class ToolLibrariesTest(unittest.TestCase):
    def test_complete_and_missing_chain(self):
        with tempfile.TemporaryDirectory(prefix='tool libraries ') as tmp:
            root=Path(tmp);libs=root/'lib';libs.mkdir()
            leaf=libs/'leaf.dylib';compat=libs/'compat.dylib';update=root/'update'
            def command(*args):
                subprocess.run(list(map(str,args)),check=True,capture_output=True)
            source=root/'test.c'
            source.write_text('int leaf(void) { return 0; }')
            command('clang','-dynamiclib',source,'-o',leaf,'-Wl,-install_name,@loader_path/leaf.dylib')
            source.write_text('extern int leaf(void); int compat(void) { return leaf(); }')
            command('clang','-dynamiclib',source,leaf,'-o',compat,
                    '-Wl,-headerpad_max_install_names','-Wl,-install_name,@loader_path/lib/compat.dylib')
            source.write_text('extern int compat(void); int main(void) { return compat(); }')
            command('clang',source,compat,'-o',update)
            firmware.check_tool_libraries(update,compat)
            command('install_name_tool','-change','@loader_path/leaf.dylib',libs/'missing.dylib',compat)
            command('codesign','--force','--sign','-',compat)
            with self.assertRaisesRegex(RuntimeError,'刷机工具缺少依赖库 missing.dylib'):
                firmware.check_tool_libraries(update,compat)
