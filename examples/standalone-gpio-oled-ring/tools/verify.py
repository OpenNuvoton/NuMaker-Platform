"""Run native UART/ring/application checks and optionally build all MCU variants."""
from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build',action='store_true',help='Also compile the three applications and both diagnostics')
args=parser.parse_args()
fixtures=[('test_header_link',[]),('test_demo_startup',[]),('test_standalone_demo',[]),
          ('test_demo_sensor_paths',[]),('test_police_strobe',[]),
          ('test_standalone_demo',['-DDEMO_FIXED_DIMMER=60']),
          ('test_demo_sensor_paths',['-DDEMO_PB5_DISPLAY'])]
with tempfile.TemporaryDirectory(prefix='numaker-native-') as directory:
    for i,(name,defines) in enumerate(fixtures):
        output=Path(directory)/str(i)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*defines,
                        str(ROOT/'tools'/f'{name}.c'),'-o',str(output)],check=True,cwd=ROOT)
        subprocess.run([str(output)],check=True,cwd=ROOT)
subprocess.run([sys.executable,str(ROOT/'tools/test_header_ring.py')],check=True,cwd=ROOT)
if args.build:
    profiles=[['--profile','sensor'],['--profile','dt57','--relay-active-level','0'],
              ['--profile','lighting','--swap-green-blue'],
              ['--profile','sensor','--demo-pb5-display'],
              ['--profile','dt57','--relay-active-level','0','--demo-fixed-dimmer','60']]
    for profile in profiles:
        subprocess.run([sys.executable,str(ROOT/'tools/build.py'),'--standalone-demo',*profile],check=True,cwd=ROOT)
print('PASS: native verification'+(' and five MCU builds' if args.build else ''))
