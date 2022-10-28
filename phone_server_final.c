#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <complex.h>
#include <assert.h>
#include <math.h>
#include <string.h>

#define N 1024
#define NUM_THREAD 2

//非同期処理に加えてbandpassフィルタを加える
//ここからbadpassフィルタに必要な関数

typedef short sample_t;

void die(char * s) {
    perror(s);
    exit(1);
}

ssize_t read_n(int fd, ssize_t n, void * buf) {
    ssize_t re = 0;
    while (re < n) {
        ssize_t r = read(fd, buf + re, n - re);
        if (r == -1) die("read");
        if (r == 0) break;
        re += r;
    }
    memset(buf + re, 0, n - re);
    return re;
}

ssize_t write_n(int fd, ssize_t n, void * buf) {
    ssize_t wr = 0;
    while (wr < n) {
        ssize_t w = write(fd, buf + wr, n - wr);
        if (w == -1) die("write");
        wr += w;
    }
    return wr;
}

void sample_to_complex(sample_t * s, double complex * X, long n) {
    long i;
    for (i = 0; i < n; i++) X[i] = s[i];
}

void complex_to_sample(double complex * X, sample_t * s, long n) {
    long i;
    for (i = 0; i < n; i++) {
        s[i] = creal(X[i]);
    }
}

void fft_r(double complex * x, double complex * y, long n, double complex w) {
    if (n == 1) { y[0] = x[0]; }
    else {
        complex double W = 1.0;
        long i;
        for (i = 0; i < n/2; i++) {
            y[i]     =     (x[i] + x[i+n/2]); /* 偶数行 */
            y[i+n/2] = W * (x[i] - x[i+n/2]); /* 奇数行 */
            W *= w;
        }
        fft_r(y,     x,     n/2, w * w);
        fft_r(y+n/2, x+n/2, n/2, w * w);
        for (i = 0; i < n/2; i++) {
            y[2*i]   = x[i];
            y[2*i+1] = x[i+n/2];
        }
    }
}

void fft(double complex * x, double complex * y, long n) {
    long i;
    double arg = 2.0 * M_PI / n;
    complex double w = cos(arg) - 1.0j * sin(arg);
    fft_r(x, y, n, w);
    for (i = 0; i < n; i++) y[i] /= n;
}

void ifft(double complex * y, double complex * x, long n) {
    double arg = 2.0 * M_PI / n;
    complex double w = cos(arg) + 1.0j * sin(arg);
    fft_r(y, x, n, w);
}

int pow2check(long _N) {
    long n = _N;
    while (n > 1) {
        if (n % 2) return 0;
        n = n / 2;
    }
    return 1;
}

void print_complex(FILE * wp, double complex * Y, long n) {
    long i;
    for (i = 0; i < n; i++) {
        fprintf(wp, "%ld %f %f %f %f\n",
                i,
                creal(Y[i]), cimag(Y[i]),
                cabs(Y[i]), atan2(cimag(Y[i]), creal(Y[i])));
    }
}

void bandpass(double complex * y, int min, int max, long n) {
    for (int i = 0; i < n; i++) {
        if (44100*i < min*n || max*n < 44100*i) {
            y[i] = 0;
        }
    }
}

//ここまでbandpassに必要な関数
//ここから非同期処理のコード

typedef struct args{
    int fd;
    char *data;
    int s;
    int number;
    int min;
    int max;
} Args;

void *receive_t(void * ptr) {
    Args* args = (Args*) ptr;
    ssize_t n3;
    while (1) {
        sample_t * buf = calloc(sizeof(sample_t), args->number);
        n3 = read_n(args->s, N, buf);
        if (n3==0) break;
        double complex * X = calloc(sizeof(complex double), args->number);
        double complex * Y = calloc(sizeof(complex double), args->number);

        sample_to_complex(buf, X, args->number);
        fft(X, Y, args->number);
        bandpass(Y, args->min, args->max, args->number);
        ifft(Y, X, args->number);
        complex_to_sample(X, buf, args->number);
        write(1, buf, n3);

        free(buf);
        free(X);
        free(Y);
    }
}

void *send_t(void * ptr) {
    Args* args = (Args*) ptr;
    ssize_t n2;
    while(1) {
        n2=read(args->fd, args->data, N);
        int n = send(args->s, args->data, n2, 0);
        if (n == -1) {
            printf("send error(%s)\n", strerror(errno));
            exit(1);
        }
    }
}

//ここまで非同期処理のコード

int main(int argc, char *argv[]) {
    int ss = socket(PF_INET, SOCK_STREAM, 0);
    if (ss == -1) {// error: print error
        printf("socket error(%s)\n", strerror(errno));
        exit(1);
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(atoi(argv[1]));
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(ss, (struct sockaddr *) &addr, sizeof(addr));

    listen(ss, 10);

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(struct sockaddr_in);
    int s = accept(ss, (struct sockaddr *) &client_addr, &len);
    if (s == -1) {
        perror("open"); //stdio.h, errno.h
        exit(1); //stdlib.h
    }
    close(ss);


    char data_s[N];
    char data_r[N];
    FILE *fp = popen("rec -t raw -b 16 -c 1 -e s -r 44100 - ", "r");
    int fd = fileno(fp);

    pthread_t t[NUM_THREAD];
    Args d[NUM_THREAD];
    d[0].fd = fd;
    d[0].data = data_r;
    d[0].s = s;
    d[0].min = 200;
    d[0].max = 5000;
    d[0].number = 8192;
    pthread_create(&t[0], NULL, receive_t, &d[0]);

    d[1].fd = fd;
    d[1].data = data_s;
    d[1].s = s;
    d[1].min = 200;
    d[1].max = 5000;
    d[1].number = 8192;
    pthread_create(&t[1], NULL, send_t, &d[1]);

    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);

    close(s);
    return 0;
}