/* metricas.c - Métricas de espera y rendimiento (entregable 6).
 *
 * Corre la versión correcta con distintas combinaciones de recursos (M) y
 * workers, sin pausas artificiales dentro del servicio. Cada worker hace
 * 100 operaciones: 20 % consultas y 80 % reservar -> usar (0..300 us) ->
 * cancelar, con timeout de 20 ms. Se repite 5 veces cada caso.
 *
 * Reporta: espera promedio, p95 y máxima de cada reserva, porcentaje de
 * reservas que se bloquearon y que vencieron su timeout, ocupación máxima
 * y operaciones por segundo.
 *
 * Uso: ./bin/metricas
 */
#include <sys/stat.h>

#include "carga.h"

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const int recursos[] = {1, 3, 10};
    const int hilos[] = {8, 16, 32};
    const int reps = 5;
    mkdir("resultados", 0755);
    FILE *csv = fopen("resultados/metricas.csv", "w");
    if (csv)
        fprintf(csv, "M,workers,espera_prom_ms,espera_p95_ms,espera_max_ms,pct_bloqueadas,"
                     "pct_timeouts,ocupacion_max,ops_por_s,corridas_con_falla\n");

    printf("%3s %7s %10s %10s %10s %9s %9s %8s %10s %7s\n", "M", "workers", "espera_ms",
           "p95_ms", "max_ms", "%bloq", "%timeout", "ocup_max", "ops/s", "fallas");
    for (size_t a = 0; a < sizeof recursos / sizeof *recursos; ++a) {
        for (size_t b = 0; b < sizeof hilos / sizeof *hilos; ++b) {
            config_carga_t cfg = config_por_defecto();
            cfg.M = recursos[a];
            cfg.workers = hilos[b];
            cfg.ops = 100;
            cfg.timeout_ms = 20;
            cfg.uso_us = 300;
            cfg.pausa_us = 0;
            double prom = 0, p95 = 0, max = 0, bloq = 0, tout = 0, tps = 0;
            int ocup = 0, fallas = 0;
            for (int rep = 0; rep < reps; ++rep) {
                cfg.semilla = 9000u + 31u * (unsigned)rep;
                resultado_carga_t r = ejecutar_carga(&OPS_SEGURO, &cfg);
                long intentos = r.operaciones - r.consultas;
                prom += r.espera_prom_ms;
                p95 += r.espera_p95_ms;
                if (r.espera_max_ms > max) max = r.espera_max_ms;
                bloq += 100.0 * (double)r.esperas / (double)intentos;
                tout += 100.0 * (double)r.timeouts / (double)intentos;
                tps += r.throughput;
                if (r.ocupacion_max > ocup) ocup = r.ocupacion_max;
                if (!r.ok) fallas++;
                liberar_resultado(&r);
            }
            prom /= reps; p95 /= reps; bloq /= reps; tout /= reps; tps /= reps;
            printf("%3d %7d %10.3f %10.3f %10.3f %8.1f%% %8.1f%% %8d %10.0f %4d/%d\n", cfg.M,
                   cfg.workers, prom, p95, max, bloq, tout, ocup, tps, fallas, reps);
            if (csv)
                fprintf(csv, "%d,%d,%.4f,%.4f,%.4f,%.2f,%.2f,%d,%.0f,%d\n", cfg.M, cfg.workers,
                        prom, p95, max, bloq, tout, ocup, tps, fallas);
        }
    }
    if (csv) fclose(csv);
    printf("\nCSV guardado en: resultados/metricas.csv\n");
    return 0;
}
