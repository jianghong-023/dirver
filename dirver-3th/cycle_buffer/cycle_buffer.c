#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>


/* ע��:
 * �����man���ҵ�pthread_mutex_*����ʱ����Ҫ��װ
 * sudo apt-get install manpages-posix-dev
 */

/*
 * cycle_buffer.c
 * ���λ���������
 *
 */

#define MAXSIZE (1024 * 1024)

#define min( x, y ) ( (x) < (y) ? (x): (y) )


struct cycle_buf_t {
	uint32_t	size;
	uint32_t	in;
	uint32_t	out;
	uint8_t		*buf;
	pthread_mutex_t rw_lock;
};

/* ��ʼ�� */
static struct cycle_buf_t* cycle_init(void)
{
	struct cycle_buf_t* cycle_p;
	cycle_p = calloc( 1, sizeof(*cycle_p) );
	if ( !cycle_p )
	{
		exit( 1 );
	}

	cycle_p->buf = calloc( 1, MAXSIZE );
	if ( !cycle_p->buf )
	{
		free( cycle_p );
		exit( 1 );
	}

	cycle_p->size	= MAXSIZE;
	cycle_p->in	= cycle_p->out = 0;
	pthread_mutex_init( &cycle_p->rw_lock, NULL );

	return(cycle_p);
}


/* д������ */
static int32_t in_buf( struct cycle_buf_t* cycle_p, uint8_t *date, uint32_t lenth )
{
	int32_t		ret = -1;
	uint32_t	re_len, w_len;
	if ( !cycle_p )
		return(ret);

/* ʣ���С */
	re_len = min( lenth, (cycle_p->size - (cycle_p->in + cycle_p->out)) );
/* Ӧд��С */
	w_len = min( re_len, (cycle_p->size - ((cycle_p->size - 1) & cycle_p->in) ));
printf("w_len: %d\n",w_len);
/* дλ������ */
	memcpy( cycle_p->buf + ((cycle_p->size - 1) & cycle_p->in), date, w_len );

/* ���д��β������û��д�꣬��ô��ͷ����ʼдʣ��� */
	memcpy( cycle_p->buf, date + 1, lenth-w_len);

	cycle_p->in += lenth;

	return(lenth);
}

/* ������� */
static int32_t out_buf( struct cycle_buf_t* cycle_p, uint8_t *date, uint32_t lenth )
{
	int32_t		ret = -1;
	uint32_t	re_len, r_len;

	if ( !cycle_p )
		return(ret);

/* ���ݴ�С */
	re_len = min( lenth, (cycle_p->in - cycle_p->out) );
printf("in: %d  out :%d  re_len:%d \n",cycle_p->in,cycle_p->out,re_len);
/* Ӧ����С */
	r_len = min( re_len, (cycle_p->size - ((cycle_p->size - 1) & cycle_p->out)) );
printf("r_len: %d\n",r_len);
/* ��λ������ */
	memcpy( date, cycle_p->buf + ((cycle_p->size - 1) & cycle_p->out), r_len );

/* �������β������û�ж��꣬��ô��ͷ����ʼ��ʣ��� */
	memcpy( date +1, cycle_p->buf, lenth-r_len);

	cycle_p->out += r_len;

	return(r_len);
}


static void * start_write( void *agrv )
{
	pthread_setcancelstate( PTHREAD_CANCEL_ENABLE, NULL );
	
	struct cycle_buf_t* cycle_p = (struct cycle_buf_t *)agrv;

	uint8_t *buf = "test cycle buffer";

	pthread_detach(pthread_self()); 
	
	while ( 1 )
	{
		pthread_mutex_lock(&cycle_p->rw_lock);
		in_buf(cycle_p, buf, strlen((char *)buf) );
		printf("----\n");
		pthread_mutex_unlock(&cycle_p->rw_lock);
		usleep( 50000 );
	}

	return NULL;
}


static void * start_read( void *agrv )
{
	uint8_t buf[1024];
	int count,i =0;
	struct cycle_buf_t* cycle_p = (struct cycle_buf_t *)agrv;
	pthread_detach(pthread_self()); 
	while ( 1 )
	{
		pthread_mutex_lock(&cycle_p->rw_lock);
		count = out_buf(cycle_p, buf, sizeof(buf) );
		pthread_mutex_unlock(&cycle_p->rw_lock);
		printf("\n");
		write(1,buf,count);
		printf("%d \n",i++);
		usleep( 50000 );
	}

	return NULL;
}


int main( void )
{
	int		result;
	pthread_t	pthr1, pthr2;

	struct cycle_buf_t* cyle_p = cycle_init();

	if(!cyle_p)
		return -1;

	result = pthread_create( &pthr1, NULL, start_write, cyle_p );
	if ( result < 0 )
	{
		fprintf( stderr, "pthread_create() %s", strerror( errno ) );
		exit( 1 );
	}
	result = pthread_create( &pthr2, NULL, start_read, cyle_p );
	if ( result < 0 )
	{
		fprintf( stderr, "pthread_create() %s", strerror( errno ) );
		pthread_cancel( pthr1 );
		exit( 1 );
	}



	pthread_mutex_destroy(&cyle_p->rw_lock);

	while(1);

	exit( 0 );
}


