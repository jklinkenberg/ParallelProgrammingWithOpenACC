
#include <cuda.h>
#include <fcntl.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define I2D(ni, i, j) ((i) + (ni) * (j))

#ifndef USE_DEVICE_AFFINITY
#define USE_DEVICE_AFFINITY 0
#endif

#ifndef USE_CLOSEST_DEVICE
#define USE_CLOSEST_DEVICE 1
#endif

#pragma omp declare target
void step_kernel_cpu(int ni, int nj, double tfac, double *temp_in, double *temp_out, int dev)
{
    int i, j, i00, im10, ip10, i0m1, i0p1;
    double d2tdx2, d2tdy2;

#pragma omp target teams distribute parallel for simd collapse(2) device(dev)
    for (j = 1; j < nj - 1; j++)
    {
        for (i = 1; i < ni - 1; i++)
        {
            i00 = I2D(ni, i, j);
            im10 = I2D(ni, i - 1, j);
            ip10 = I2D(ni, i + 1, j);
            i0m1 = I2D(ni, i, j - 1);
            i0p1 = I2D(ni, i, j + 1);
            d2tdx2 = temp_in[im10] - 2 * temp_in[i00] + temp_in[ip10];
            d2tdy2 = temp_in[i0m1] - 2 * temp_in[i00] + temp_in[i0p1];
            temp_out[i00] = temp_in[i00] + tfac * (d2tdx2 + d2tdy2);
        } // end for
    } // end for

} // end kernel

#pragma omp end declare target

int main(int argc, char *argv[])
{
    int ni, nj, nstep;
    double tfac, *temp1_h, *temp2_h;
    int i, j, i2d, istep;
    double temp_bl, temp_br, temp_tl, temp_tr;
    int NUM_THREADS;
    struct timeval tim;
    double start, end;
    double time;
    FILE *fp;
    int fd;

    if (argc < 6)
    {
        printf("Usage: %s <num GPUs> <ni> <nj> <nstep> <output file>\n", argv[0]);
        exit(1);
    }
    NUM_THREADS = atoi(argv[1]);
    ni = atoi(argv[2]);
    nj = atoi(argv[3]);
    nstep = atoi(argv[4]);
    // fp = atos(argv[5]);

    size_t num_elements = (ni + 2) * (nj + 2);

    temp1_h = (double *)malloc(sizeof(double) * num_elements);
    temp2_h = (double *)malloc(sizeof(double) * num_elements);

    for (j = 1; j < nj + 1; j++)
    {
        for (i = 1; i < ni + 1; i++)
        {
            i2d = i + (ni + 2) * j;
            temp1_h[i2d] = 0.0001f;
            temp2_h[i2d] = 0.0001f;
        }
    }

    temp_bl = 200.0f;
    temp_br = 300.0f;
    temp_tl = 200.0f;
    temp_tr = 300.0f;

    for (i = 0; i < ni + 2; i++)
    {
        j = 0;
        i2d = i + (ni + 2) * j;
        temp1_h[i2d] = temp_bl + (temp_br - temp_bl) * (double)i / (double)(ni + 1);

        j = nj + 1;
        i2d = i + (ni + 2) * j;
        temp1_h[i2d] = temp_tl + (temp_tr - temp_tl) * (double)i / (double)(ni + 1);
    }

    for (j = 0; j < nj + 2; j++)
    {
        i = 0;
        i2d = i + (ni + 2) * j;
        temp1_h[i2d] = temp_bl + (temp_tl - temp_bl) * (double)j / (double)(nj + 1);

        i = ni + 1;
        i2d = i + (ni + 2) * j;
        temp1_h[i2d] = temp_br + (temp_tr - temp_br) * (double)j / (double)(nj + 1);
    }
    memcpy(temp2_h, temp1_h, sizeof(double) * num_elements);

    tfac = 0.2;

    gettimeofday(&tim, NULL);
    start = tim.tv_sec + (tim.tv_usec / 1000000.0);

    omp_set_num_threads(NUM_THREADS);
    int rows, LDA;

    rows = nj / NUM_THREADS;
    LDA = ni + 2;

    int numAvailDevices = omp_get_num_devices();
    int numDevices = numAvailDevices;
    // int numDevices = NUM_THREADS;
    int chosenDev;

    printf("Number of devices available is : %d\n", numAvailDevices);
    printf("Number of devices used is : %d\n", numDevices);

    srand(gettimeofday(&tim, NULL));
#pragma omp parallel private(istep, chosenDev)
    {
        double *temp1, *temp2, *temp_tmp;
        int tid = omp_get_thread_num();
        int devices_in_order[numAvailDevices];
        int n_dev_found;

#if USE_DEVICE_AFFINITY
        n_dev_found = omp_get_devices_in_order(numAvailDevices, devices_in_order);
#if USE_CLOSEST_DEVICE
        chosenDev = devices_in_order[0];
#else
        chosenDev = devices_in_order[n_dev_found - 1];
#endif // USE_CLOSEST_DEVICE
#else
        chosenDev = tid % numDevices; // tid modulo number of devices
#endif // USE_DEVICE_AFFINITY

        // TODO: Check how host can also participate in computation
        // acc_set_device_num(tid+1, acc_device_not_host);
        temp1 = temp1_h + tid * rows * LDA;
        temp2 = temp2_h + tid * rows * LDA;

        printf("ThreadID %d \t Device %d running %ul part of temperature array \n", tid, chosenDev, tid * rows * LDA);
#pragma omp target data map(tofrom : temp1[0 : (rows + 2) * LDA], temp2[0 : (rows + 2) * LDA]) device(chosenDev)
        {
            for (istep = 0; istep < nstep; istep++)
            {
                step_kernel_cpu(ni + 2, rows + 2, tfac, temp1, temp2, chosenDev);
                /* update the lower halo to the host except the last device*/
                if (tid != NUM_THREADS - 1)
                {
#pragma omp target update from(temp2[rows * LDA : LDA])
                }
                /*update the upper halo to the host except the first device*/
                if (tid != 0)
                {
#pragma omp target update from(temp2[LDA : LDA])
                }

                /*make sure another device has already updated the data into host*/
#pragma omp barrier
                /* update the upper halo to the other device */
                // exchange
                // assume rows per process is a multiple of rows per device , rows per process ;

                if (tid != 0)
                {
#pragma omp target update to(temp2[0 : LDA])
                }

                if (tid != NUM_THREADS - 1)
                {
#pragma omp target update to(temp2[(rows + 1) * LDA : LDA])
                }

                temp_tmp = temp1;
                temp1 = temp2;
                temp2 = temp_tmp;
            }

            /*update the final result to host*/
            // #pragma acc update host(temp1[LDA:rows*LDA])
#pragma omp target update from(temp1[LDA : rows * LDA])
        }
    }

    gettimeofday(&tim, NULL);
    end = tim.tv_sec + (tim.tv_usec / 1000000.0);
    printf("Time for computing: %f s\n", end - start);

    /* output temp1 to a binary file */

    //    fd = create(argv[5], 00666);
    // fd = open(argv[5], O_WRONLY);
    // write(fd, temp1_h, (ni+2)*(nj+2)*sizeof(double));
    // close(fd);

    /* output temp1 to a text file */

    // fp = fopen(argv[5], "w");
    // fprintf(fp, "%d %d\n", ni, nj);
    // for (j = 0; j < nj; j++)
    // {
    //     for (i = 0; i < ni; i++)
    //     {
    //         fprintf(fp, "%d \t %d \t %.4f\n", j, i, temp1_h[i + ni * j]);
    //     }
    // }
    // fclose(fp);

} // end main
