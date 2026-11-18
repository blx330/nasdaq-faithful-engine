"""Shared chart style: thin marks, recessive axes, fixed categorical order."""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
NEUTRAL = "#a3a29c"


def setup():
    plt.rcParams.update({
        "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
        "axes.edgecolor": GRID, "axes.labelcolor": TEXT2, "xtick.color": TEXT2, "ytick.color": TEXT2,
        "text.color": TEXT, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
        "axes.spines.top": False, "axes.spines.right": False, "axes.spines.left": False,
        "lines.linewidth": 2, "lines.markersize": 6, "font.size": 10, "axes.titlesize": 12,
        "axes.titleweight": "bold", "axes.titlelocation": "left", "legend.frameon": False,
    })


def line_with_band(ax, x, y, lo, hi, color, label):
    ax.fill_between(x, lo, hi, color=color, alpha=0.15, linewidth=0)
    ax.plot(x, y, color=color, marker="o", markeredgecolor=SURFACE, markeredgewidth=2, markersize=8, label=label)
    ax.annotate(label, (x[-1], y[-1]), xytext=(8, 0), textcoords="offset points", va="center", color=TEXT2)
