"""
plot_heap_aging.py  —  v4

Two panels:
  top:    RSS over real elapsed time
  bottom: scan latency — rolling median (thick) + clipped scatter (thin, alpha)
          y-axis clipped at 95th percentile so outliers don't compress the story
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import os

FILES = {
    'system':   'results_system.tsv',
    'jemalloc': 'results_jemalloc.tsv',
    'tcmalloc': 'results_tcmalloc.tsv',
}
COLORS = {'system': '#4878CF', 'jemalloc': '#E68A2E', 'tcmalloc': '#3DA35D'}

dfs = {}
for label, path in FILES.items():
    if os.path.exists(path):
        dfs[label] = pd.read_csv(path, sep='\t')
    else:
        print(f"[warn] {path} not found, skipping")

if not dfs:
    print("No TSV files found.")
    raise SystemExit(1)

# --- compute clip threshold: 95th pct across all allocators ---
all_latency = pd.concat([df['scan_ns_per_obj'] for df in dfs.values()])
clip_95 = np.percentile(all_latency, 95)
clip_99 = np.percentile(all_latency, 99)
print(f"Latency 95th pct: {clip_95:.1f} ns  99th pct: {clip_99:.1f} ns  max: {all_latency.max():.1f} ns")
y_max = clip_95 * 1.15   # headroom above 95th pct

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 7), sharex=True)
fig.suptitle('Heap aging: RSS and scan latency under sustained mixed-size churn',
             fontsize=13, fontweight='bold')

for label, df in dfs.items():
    c = COLORS.get(label)
    x = df['elapsed_s']

    # --- RSS ---
    ax1.plot(x, df['rss_kb'] / 1024, label=label, color=c, linewidth=1.8)

    # --- Latency: rolling median (thick) + scatter clipped at y_max ---
    win = max(5, len(df) // 30)
    smooth = df['scan_ns_per_obj'].rolling(win, center=True, min_periods=1).median()

    # scatter only points below clip so they don't hide the median line
    mask = df['scan_ns_per_obj'] <= y_max
    ax2.scatter(x[mask], df['scan_ns_per_obj'][mask],
                color=c, alpha=0.12, s=3, zorder=1)

    ax2.plot(x, smooth.clip(upper=y_max), color=c, linewidth=2.2,
             label=label, zorder=3)

# annotate how many points were clipped
n_clipped = int((all_latency > y_max).sum())
pct_clipped = 100 * n_clipped / len(all_latency)
ax2.text(0.99, 0.97,
         f"{n_clipped} points ({pct_clipped:.1f}%) above y-axis limit ({y_max:.0f} ns/obj)",
         transform=ax2.transAxes, ha='right', va='top',
         fontsize=8, color='#666666', style='italic')

# --- RSS panel cosmetics ---
ax1.set_ylabel('RSS (MB)', fontsize=11)
ax1.legend(fontsize=10, loc='center right')
ax1.yaxis.set_major_formatter(ticker.FormatStrFormatter('%.0f'))
ax1.grid(axis='y', linestyle='--', alpha=0.4)
ax1.set_title('Resident set size  —  flat lines = bounded working set  |  '
              'gap between allocators = arena reservation strategy',
              fontsize=9, style='italic')

# --- latency panel cosmetics ---
ax2.set_ylim(0, y_max)
ax2.set_ylabel('Scan ns / object', fontsize=11)
ax2.set_xlabel('Elapsed real seconds', fontsize=11)
ax2.legend(fontsize=10, loc='upper right')
ax2.grid(axis='y', linestyle='--', alpha=0.4)
ax2.set_title('Hot-set scan latency  (rolling median + raw scatter, y clipped at 95th pct)  '
              '—  rising median = fragmentation degrading cache locality',
              fontsize=9, style='italic')

plt.tight_layout()
outfile = 'heap_aging.png'
plt.savefig(outfile, dpi=150)
print(f"Saved {outfile}")
