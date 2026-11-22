# 生成 Canton 测试字体：从 Noto Sans 的可变字体取出常规、粗体、斜体三个字面，只留夹具用得到的字符，
# 族名改为 Canton Test Sans。Noto Sans 用 SIL OFL 1.1 授权且没有保留字体名，许可证随字体放在同目录的 OFL.txt。
# 像素基线里的文字只用这套字体，不依赖系统字体。
# 运行：python tests/fixtures/fonts/generate.py（需要 fontTools；源字体下载到 .canton/cache/fonts 并校验摘要）
import hashlib
import urllib.parse
import urllib.request
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools import subset

OUT = Path(__file__).parent
ROOT = OUT.parents[2]
CACHE = ROOT / ".canton" / "cache" / "fonts"
SHOWCASE = OUT.parent / "showcase" / "public" / "fonts"
# google/fonts 里 Noto Sans 2.015 所在的提交
SOURCE = "https://raw.githubusercontent.com/google/fonts/8b0a1d0f5983c89bc2b93f1b5fb55f9e252744b5/ofl/notosans/"
FILES = {
    "NotoSans[wdth,wght].ttf": "bfb7bb691513f12e734dc346c03a03f784912432d7e3fa8e56efcf906fe86b3d",
    "NotoSans-Italic[wdth,wght].ttf": "58e6e0ebd1931b29a365aa2d3e2ee9a9e831a3af7cf3ad1462d4e72154f0b291",
    "OFL.txt": "cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a",
}
FAMILY = "Canton Test Sans"
# 可打印 ASCII、不换行空格与省略号（DirectWrite 的省略标记）
CHARACTERS = [*range(0x20, 0x7F), 0xA0, 0x2026]


def fetch(name: str) -> Path:
    path = CACHE / name
    if not path.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(SOURCE + urllib.parse.quote(name)) as response:
            path.write_bytes(response.read())
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != FILES[name]:
        raise SystemExit(f"{name} 的摘要不符：{digest}")
    return path


def rename(font: TTFont, style: str) -> None:
    table = font["name"]
    postscript = f"CantonTestSans-{style}"
    # 只改族名相关的条目，版权、许可证说明和链接原样保留
    for record in list(table.names):
        if record.nameID in (1, 2, 3, 4, 6, 16, 17, 21, 22, 25):
            table.removeNames(nameID=record.nameID)
    for nameID, value in ((1, FAMILY), (2, style), (3, f"{postscript};Noto Sans subset"), (4, f"{FAMILY} {style}"), (6, postscript)):
        table.setName(value, nameID, 3, 1, 0x409)
        table.setName(value, nameID, 1, 0, 0)


def build(source: Path, style: str, weight: int) -> None:
    font = instantiateVariableFont(TTFont(source), {"wght": weight, "wdth": 100}, updateFontNames=False)
    options = subset.Options()
    options.hinting = False
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=CHARACTERS)
    subsetter.subset(font)
    rename(font, style)
    os2 = font["OS/2"]
    os2.usWeightClass = weight
    italic, bold = style == "Italic", style == "Bold"
    os2.fsSelection = (os2.fsSelection & ~0x61) | (0x01 if italic else 0) | (0x20 if bold else 0) | (0x40 if not italic and not bold else 0)
    font["head"].macStyle = (1 if bold else 0) | (2 if italic else 0)
    name = f"CantonTestSans-{style}.ttf"
    font.save(OUT / name)
    # showcase 夹具经资源获取层加载同一份字体，放一份到它的 public 目录
    SHOWCASE.mkdir(parents=True, exist_ok=True)
    (SHOWCASE / name).write_bytes((OUT / name).read_bytes())


upright, italic = fetch("NotoSans[wdth,wght].ttf"), fetch("NotoSans-Italic[wdth,wght].ttf")
build(upright, "Regular", 400)
build(upright, "Bold", 700)
build(italic, "Italic", 400)
license = fetch("OFL.txt").read_bytes().replace(b"\r\n", b"\n")
for directory in (OUT, SHOWCASE):
    (directory / "OFL.txt").write_bytes(license)
