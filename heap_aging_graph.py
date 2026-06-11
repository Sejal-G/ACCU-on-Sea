import pandas as pd
import matplotlib.pyplot as plt

files = {
    'system':  'results_system.tsv',
    'jemalloc':'results_jemalloc.tsv',
    'tcmalloc':'results_tcmalloc.tsv',
}

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)

for label, path in files.items():
    df = pd.read_csv(path, sep='\t')
    ax1.plot(df['day'], df['rss_kb'] / 1024, label=label)
    ax2.plot(df['day'], df['scan_ns_per_obj'], label=label)

ax1.set_ylabel('RSS (MB)')
ax1.set_title('Heap fragmentation over 5 simulated days')
ax1.legend()

ax2.set_ylabel('Scan ns / object')
ax2.set_xlabel('Simulated day')

plt.tight_layout()
plt.savefig('heap_aging.png', dpi=150)