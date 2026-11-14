Plots for the xv6 buffer-cache benchmark
=========================================
python3 plot_results.py            # reads results.csv -> plots/fig1..fig9
python3 plot_results.py --lookup list --out plots_list
python3 trace_plot.py --src ../xv6-lru --nbuf 60 --workload 2 --rounds 1 --trace 400   # fig10

results.csv comes from: bash compare_bcache.sh   (re-run it, then re-plot)
Edit the "EDIT ME" block at the top of each script: colours, NBUF list, which workload
configs appear (CONFIGS), the spotlight config for the cost-breakdown figure.
Needs: python3, matplotlib, pandas, numpy (trace_plot.py also needs gcc).
