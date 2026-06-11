"""
plot_heap_aging.py  —  v3

Reads results_system.tsv, results_jemalloc.tsv, results_tcmalloc.tsv
and produces heap_aging.png with two panels:
  top:    RSS over real elapsed time
  bottom: scan latency (ns/object) over real elapsed time

Run:  python3 plot_heap_aging.py
"""

import pandas as pd
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

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 7), sharex=True)
fig.suptitle('Heap aging: RSS and scan latency under sustained mixed-size churn',
             fontsize=13, fontweight='bold')

for label, df in dfs.items():
    c = COLORS.get(label, None)
    ax1.plot(df['elapsed_s'], df['rss_kb'] / 1024, label=label, color=c, linewidth=1.5)
    # Rolling median to smooth scheduler noise; window ~5% of rows
    win = max(3, len(df) // 20)
    smooth = df['scan_ns_per_obj'].rolling(win, center=True, min_periods=1).median()
    ax2.plot(df['elapsed_s'], smooth, label=label, color=c, linewidth=1.5)
    ax2.scatter(df['elapsed_s'], df['scan_ns_per_obj'],
                color=c, alpha=0.15, s=4, zorder=1)

ax1.set_ylabel('RSS (MB)', fontsize=11)
ax1.legend(fontsize=10)
ax1.yaxis.set_major_formatter(ticker.FormatStrFormatter('%.1f'))
ax1.grid(axis='y', linestyle='--', alpha=0.4)
ax1.set_title('Resident set size  (flat = bounded working set, gap = arena overhead)',
              fontsize=10, style='italic')

ax2.set_ylabel('Scan ns / object', fontsize=11)
ax2.set_xlabel('Elapsed real seconds', fontsize=11)
ax2.grid(axis='y', linestyle='--', alpha=0.4)
ax2.set_title('Hot-set scan latency  (rolling median + raw scatter)  '
              '— rising trend = fragmentation degrading cache locality',
              fontsize=10, style='italic')

plt.tight_layout()
plt.savefig('heap_aging.png', dpi=150)
print("Saved heap_aging.png")
