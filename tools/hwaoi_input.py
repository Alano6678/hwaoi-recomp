"""Send one expiring diagnostic input command to an opt-in HWAoI runtime."""
import argparse
import os
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BUTTONS={name:1<<i for i,name in enumerate(("A","B","X","Y","LCLICK","RCLICK","L","R","ZL","ZR","PLUS","MINUS","LEFT","UP","RIGHT","DOWN"))}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--feed",type=Path,required=True)
    parser.add_argument("--buttons",nargs="*",choices=tuple(BUTTONS),default=[])
    parser.add_argument("--left",nargs=2,type=int,default=(0,0))
    parser.add_argument("--right",nargs=2,type=int,default=(0,0))
    parser.add_argument("--hold-ms",type=int,default=200)
    parser.add_argument("--fps",type=int,choices=(30,60,90,120),default=0,help="requires experimental rate control in the runtime")
    parser.add_argument("--output",nargs=2,type=int,default=(0,0),metavar=("WIDTH","HEIGHT"),help="output window size; does not patch internal rendering")
    args=parser.parse_args()
    path=args.feed.resolve()
    if not path.is_relative_to(ROOT/"work/reports") or path==ROOT/"work/reports":
        parser.error("feed must be a file under ignored work/reports")
    if not 0<=args.hold_ms<=10000 or any(not -32767<=v<=32767 for v in (*args.left,*args.right)):
        parser.error("hold must be 0..10000 ms; axes must be -32767..32767")
    if any(args.output) and not (640<=args.output[0]<=3840 and 360<=args.output[1]<=2160):
        parser.error("output must be 640..3840 by 360..2160")
    sequence=1
    if path.exists():
        sequence=int(path.read_text(encoding="ascii").split()[0])+1
    if not 0<sequence<2**64: parser.error("input sequence exhausted")
    path.parent.mkdir(parents=True,exist_ok=True)
    mask=0
    for button in args.buttons:mask|=BUTTONS[button]
    line=f"{sequence} {mask:x} {args.left[0]} {args.left[1]} {args.right[0]} {args.right[1]} {args.hold_ms} {args.fps} {args.output[0]} {args.output[1]}\n"
    temp=path.with_name(path.name+f".{os.getpid()}.tmp")
    temp.write_text(line,encoding="ascii")
    try:os.replace(temp,path)
    finally:
        if temp.exists():temp.unlink()
    print(line.strip())


if __name__=="__main__":main()
