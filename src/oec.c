// ec - echo canceller

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#include "conf.h"
#include "audio.h"
#include "oslec.h"
#include "fir_new.h"
#include "bit_operations.h"

#define DC_LOG2BETA			3	/* log2() of DC filter Beta */
#define MIN_TX_POWER_FOR_ADAPTION	64
#define MIN_RX_POWER_FOR_ADAPTION	64
#define DTD_HANGOVER			600	/* 600 samples, or 75ms     */

/*!
    G.168 echo canceller descriptor. This defines the working state for a line
    echo canceller.
*/
struct oslec_state {
	int16_t tx, rx;
	int16_t clean;
	int16_t clean_nlp;

	int nonupdate_dwell;
	int curr_pos;
	int taps;
	int log2taps;
	int adaption_mode;

	int cond_met;
	int32_t Pstates;
	int16_t adapt;
	int32_t factor;
	int16_t shift;

	/* Average levels and averaging filter states */
	int Ltxacc, Lrxacc, Lcleanacc, Lclean_bgacc;
	int Ltx, Lrx;
	int Lclean;
	int Lclean_bg;
	int Lbgn, Lbgn_acc, Lbgn_upper, Lbgn_upper_acc;

	/* foreground and background filter states */
	fir16_state_t fir_state;
	fir16_state_t fir_state_bg;
	int16_t *fir_taps16[2];

	/* DC blocking filter states */
	int tx_1, tx_2, rx_1, rx_2;

	/* optional High Pass Filter states */
	int32_t xvtx[5], yvtx[5];
	int32_t xvrx[5], yvrx[5];

	/* Parameters for the optional Hoth noise generator */
	int cng_level;
	int cng_rndnum;
	int cng_filter;

	/* snapshot sample of coeffs used for development */
	int16_t *snapshot;
};

static inline void lms_adapt_bg(struct oslec_state *ec, int clean,				    int shift)
{
	int i;

	int offset1;
	int offset2;
	int factor;
	int exp;

	if (shift > 0)
		factor = clean << shift;
	else
		factor = clean >> -shift;

	/* Update the FIR taps */

	offset2 = ec->curr_pos;
	offset1 = ec->taps - offset2;

	for (i = ec->taps - 1; i >= offset1; i--) {
		exp = (ec->fir_state_bg.history[i - offset1] * factor);
		ec->fir_taps16[1][i] += (int16_t) ((exp + (1 << 14)) >> 15);
	}
	for (; i >= 0; i--) {
		exp = (ec->fir_state_bg.history[i + offset2] * factor);
		ec->fir_taps16[1][i] += (int16_t) ((exp + (1 << 14)) >> 15);
	}
}


const char *usage =
    "Usage:\n %s [options]\n"
    "Options:\n"
    " -i PCM            playback PCM (default)\n"
    " -o PCM            capture PCM (default)\n"
    " -r rate           sample rate (16000)\n"
    " -c channels       recording channels (2)\n"
    " -b size           buffer size (262144)\n"
    " -d delay          system delay between playback and capture (0)\n"
    " -f filter_length  AEC filter length (2048)\n"
    " -s                save audio to /tmp/playback.raw, /tmp/recording.raw and /tmp/out.raw\n"
    " -D                daemonize\n"
    " -h                display this help text\n"
    "Note:\n"
    " Access audio I/O through named pipes (/tmp/ec.input for playback and /tmp/ec.output for recording)\n"
    "  `cat audio.raw > /tmp/ec.input` to play audio\n"
    "  `cat /tmp/ec.output > out.raw` to get recording audio\n"
    " Only support mono playback\n";

volatile int g_is_quit = 0;
struct oslec_state *oslec;
extern int fifo_setup(conf_t *conf);
extern int fifo_write(void *buf, size_t frames);

void int_handler(int signal)
{
    printf("Caught signal %d, quit...\n", signal);

    g_is_quit = 1;
}

////// oslec
struct oslec_state *oslec_create(int len, int adaption_mode)
{
	struct oslec_state *ec;
	int i;

	ec = calloc(1, sizeof(*ec));
	if (!ec)
		return NULL;

	ec->taps = len;
	ec->log2taps = top_bit(len);
	ec->curr_pos = ec->taps - 1;

	for (i = 0; i < 2; i++) {
		ec->fir_taps16[i] =
		    calloc(ec->taps, sizeof(int16_t));
		if (!ec->fir_taps16[i])
			goto error_oom;
	}

	fir16_create(&ec->fir_state, ec->fir_taps16[0], ec->taps);
	fir16_create(&ec->fir_state_bg, ec->fir_taps16[1], ec->taps);

	for (i = 0; i < 5; i++) {
		ec->xvtx[i] = ec->yvtx[i] = ec->xvrx[i] = ec->yvrx[i] = 0;
	}

	ec->cng_level = 1000;
	oslec_adaption_mode(ec, adaption_mode);

	ec->snapshot = calloc(ec->taps, sizeof(int16_t));
	if (!ec->snapshot)
		goto error_oom;

	ec->cond_met = 0;
	ec->Pstates = 0;
	ec->Ltxacc = ec->Lrxacc = ec->Lcleanacc = ec->Lclean_bgacc = 0;
	ec->Ltx = ec->Lrx = ec->Lclean = ec->Lclean_bg = 0;
	ec->tx_1 = ec->tx_2 = ec->rx_1 = ec->rx_2 = 0;
	ec->Lbgn = ec->Lbgn_acc = 0;
	ec->Lbgn_upper = 200;
	ec->Lbgn_upper_acc = ec->Lbgn_upper << 13;

	return ec;

      error_oom:
	for (i = 0; i < 2; i++)
		free(ec->fir_taps16[i]);

	free(ec);
	return NULL;
}

void oslec_free(struct oslec_state *ec)
{
	int i;

	fir16_free(&ec->fir_state);
	fir16_free(&ec->fir_state_bg);
	for (i = 0; i < 2; i++)
		free(ec->fir_taps16[i]);
	free(ec->snapshot);
	free(ec);
}

void oslec_adaption_mode(struct oslec_state *ec, int adaption_mode)
{
	ec->adaption_mode = adaption_mode;
}

void oslec_flush(struct oslec_state *ec)
{
	int i;

	ec->Ltxacc = ec->Lrxacc = ec->Lcleanacc = ec->Lclean_bgacc = 0;
	ec->Ltx = ec->Lrx = ec->Lclean = ec->Lclean_bg = 0;
	ec->tx_1 = ec->tx_2 = ec->rx_1 = ec->rx_2 = 0;

	ec->Lbgn = ec->Lbgn_acc = 0;
	ec->Lbgn_upper = 200;
	ec->Lbgn_upper_acc = ec->Lbgn_upper << 13;

	ec->nonupdate_dwell = 0;

	fir16_flush(&ec->fir_state);
	fir16_flush(&ec->fir_state_bg);
	ec->fir_state.curr_pos = ec->taps - 1;
	ec->fir_state_bg.curr_pos = ec->taps - 1;
	for (i = 0; i < 2; i++)
		memset(ec->fir_taps16[i], 0, ec->taps * sizeof(int16_t));

	ec->curr_pos = ec->taps - 1;
	ec->Pstates = 0;
}

void oslec_snapshot(struct oslec_state *ec)
{
	memcpy(ec->snapshot, ec->fir_taps16[0], ec->taps * sizeof(int16_t));
}

/* Dual Path Echo Canceller ------------------------------------------------*/

int16_t oslec_update(struct oslec_state *ec, int16_t tx, int16_t rx)
{
	int32_t echo_value;
	int clean_bg;
	int tmp, tmp1;

	/* Input scaling was found be required to prevent problems when tx
	   starts clipping.  Another possible way to handle this would be the
	   filter coefficent scaling. */

	ec->tx = tx;
	ec->rx = rx;
	tx >>= 1;
	rx >>= 1;

	/*
	   Filter DC, 3dB point is 160Hz (I think), note 32 bit precision required
	   otherwise values do not track down to 0. Zero at DC, Pole at (1-Beta)
	   only real axis.  Some chip sets (like Si labs) don't need
	   this, but something like a $10 X100P card does.  Any DC really slows
	   down convergence.

	   Note: removes some low frequency from the signal, this reduces
	   the speech quality when listening to samples through headphones
	   but may not be obvious through a telephone handset.

	   Note that the 3dB frequency in radians is approx Beta, e.g. for
	   Beta = 2^(-3) = 0.125, 3dB freq is 0.125 rads = 159Hz.
	 */

	if (ec->adaption_mode & ECHO_CAN_USE_RX_HPF) {
		tmp = rx << 15;
#if 1
		/* Make sure the gain of the HPF is 1.0. This can still saturate a little under
		   impulse conditions, and it might roll to 32768 and need clipping on sustained peak
		   level signals. However, the scale of such clipping is small, and the error due to
		   any saturation should not markedly affect the downstream processing. */
		tmp -= (tmp >> 4);
#endif
		ec->rx_1 += -(ec->rx_1 >> DC_LOG2BETA) + tmp - ec->rx_2;

		/* hard limit filter to prevent clipping.  Note that at this stage
		   rx should be limited to +/- 16383 due to right shift above */
		tmp1 = ec->rx_1 >> 15;
		if (tmp1 > 16383)
			tmp1 = 16383;
		if (tmp1 < -16383)
			tmp1 = -16383;
		rx = tmp1;
		ec->rx_2 = tmp;
	}

	/* Block average of power in the filter states.  Used for
	   adaption power calculation. */

	{
		int new, old;

		/* efficient "out with the old and in with the new" algorithm so
		   we don't have to recalculate over the whole block of
		   samples. */
		new = (int)tx *(int)tx;
		old = (int)ec->fir_state.history[ec->fir_state.curr_pos] *
		    (int)ec->fir_state.history[ec->fir_state.curr_pos];
		ec->Pstates +=
		    ((new - old) + (1 << ec->log2taps)) >> ec->log2taps;
		if (ec->Pstates < 0)
			ec->Pstates = 0;
	}

	/* Calculate short term average levels using simple single pole IIRs */

	ec->Ltxacc += abs(tx) - ec->Ltx;
	ec->Ltx = (ec->Ltxacc + (1 << 4)) >> 5;
	ec->Lrxacc += abs(rx) - ec->Lrx;
	ec->Lrx = (ec->Lrxacc + (1 << 4)) >> 5;

	/* Foreground filter --------------------------------------------------- */

	ec->fir_state.coeffs = ec->fir_taps16[0];
	echo_value = fir16(&ec->fir_state, tx);
	ec->clean = rx - echo_value;
	ec->Lcleanacc += abs(ec->clean) - ec->Lclean;
	ec->Lclean = (ec->Lcleanacc + (1 << 4)) >> 5;

	/* Background filter --------------------------------------------------- */

	echo_value = fir16(&ec->fir_state_bg, tx);
	clean_bg = rx - echo_value;
	ec->Lclean_bgacc += abs(clean_bg) - ec->Lclean_bg;
	ec->Lclean_bg = (ec->Lclean_bgacc + (1 << 4)) >> 5;

	/* Background Filter adaption ----------------------------------------- */

	/* Almost always adap bg filter, just simple DT and energy
	   detection to minimise adaption in cases of strong double talk.
	   However this is not critical for the dual path algorithm.
	 */
	ec->factor = 0;
	ec->shift = 0;
	if ((ec->nonupdate_dwell == 0)) {
		int P, logP, shift;

		/* Determine:

		   f = Beta * clean_bg_rx/P ------ (1)

		   where P is the total power in the filter states.

		   The Boffins have shown that if we obey (1) we converge
		   quickly and avoid instability.

		   The correct factor f must be in Q30, as this is the fixed
		   point format required by the lms_adapt_bg() function,
		   therefore the scaled version of (1) is:

		   (2^30) * f  = (2^30) * Beta * clean_bg_rx/P
		   factor  = (2^30) * Beta * clean_bg_rx/P         ----- (2)

		   We have chosen Beta = 0.25 by experiment, so:

		   factor  = (2^30) * (2^-2) * clean_bg_rx/P

		   (30 - 2 - log2(P))
		   factor  = clean_bg_rx 2                         ----- (3)

		   To avoid a divide we approximate log2(P) as top_bit(P),
		   which returns the position of the highest non-zero bit in
		   P.  This approximation introduces an error as large as a
		   factor of 2, but the algorithm seems to handle it OK.

		   Come to think of it a divide may not be a big deal on a
		   modern DSP, so its probably worth checking out the cycles
		   for a divide versus a top_bit() implementation.
		 */

		P = MIN_TX_POWER_FOR_ADAPTION + ec->Pstates;
		logP = top_bit(P) + ec->log2taps;
		shift = 30 - 2 - logP;
		ec->shift = shift;

		lms_adapt_bg(ec, clean_bg, shift);
	}

	/* very simple DTD to make sure we dont try and adapt with strong
	   near end speech */

	ec->adapt = 0;
	if ((ec->Lrx > MIN_RX_POWER_FOR_ADAPTION) && (ec->Lrx > ec->Ltx))
		ec->nonupdate_dwell = DTD_HANGOVER;
	if (ec->nonupdate_dwell)
		ec->nonupdate_dwell--;

	/* Transfer logic ------------------------------------------------------ */

	/* These conditions are from the dual path paper [1], I messed with
	   them a bit to improve performance. */

	if ((ec->adaption_mode & ECHO_CAN_USE_ADAPTION) &&
	    (ec->nonupdate_dwell == 0) &&
	    (8 * ec->Lclean_bg <
	     7 * ec->Lclean) /* (ec->Lclean_bg < 0.875*ec->Lclean) */ &&
	    (8 * ec->Lclean_bg <
	     ec->Ltx) /* (ec->Lclean_bg < 0.125*ec->Ltx)    */ ) {
		if (ec->cond_met == 6) {
			/* BG filter has had better results for 6 consecutive samples */
			ec->adapt = 1;
			memcpy(ec->fir_taps16[0], ec->fir_taps16[1],
			       ec->taps * sizeof(int16_t));
		} else
			ec->cond_met++;
	} else
		ec->cond_met = 0;

	/* Non-Linear Processing --------------------------------------------------- */

	ec->clean_nlp = ec->clean;
	if (ec->adaption_mode & ECHO_CAN_USE_NLP) {
		/* Non-linear processor - a fancy way to say "zap small signals, to avoid
		   residual echo due to (uLaw/ALaw) non-linearity in the channel.". */

		if ((16 * ec->Lclean < ec->Ltx)) {
			/* Our e/c has improved echo by at least 24 dB (each factor of 2 is 6dB,
			   so 2*2*2*2=16 is the same as 6+6+6+6=24dB) */
			if (ec->adaption_mode & ECHO_CAN_USE_CNG) {
				ec->cng_level = ec->Lbgn;

				/* Very elementary comfort noise generation.  Just random
				   numbers rolled off very vaguely Hoth-like.  DR: This
				   noise doesn't sound quite right to me - I suspect there
				   are some overlfow issues in the filtering as it's too
				   "crackly".  TODO: debug this, maybe just play noise at
				   high level or look at spectrum.
				 */

				ec->cng_rndnum =
				    1664525U * ec->cng_rndnum + 1013904223U;
				ec->cng_filter =
				    ((ec->cng_rndnum & 0xFFFF) - 32768 +
				     5 * ec->cng_filter) >> 3;
				ec->clean_nlp =
				    (ec->cng_filter * ec->cng_level * 8) >> 14;

			} else if (ec->adaption_mode & ECHO_CAN_USE_CLIP) {
				/* This sounds much better than CNG */
				if (ec->clean_nlp > ec->Lbgn)
					ec->clean_nlp = ec->Lbgn;
				if (ec->clean_nlp < -ec->Lbgn)
					ec->clean_nlp = -ec->Lbgn;
			} else {
				/* just mute the residual, doesn't sound very good, used mainly
				   in G168 tests */
				ec->clean_nlp = 0;
			}
		} else {
			/* Background noise estimator.  I tried a few algorithms
			   here without much luck.  This very simple one seems to
			   work best, we just average the level using a slow (1 sec
			   time const) filter if the current level is less than a
			   (experimentally derived) constant.  This means we dont
			   include high level signals like near end speech.  When
			   combined with CNG or especially CLIP seems to work OK.
			 */
			if (ec->Lclean < 40) {
				ec->Lbgn_acc += abs(ec->clean) - ec->Lbgn;
				ec->Lbgn = (ec->Lbgn_acc + (1 << 11)) >> 12;
			}
		}
	}

	/* Roll around the taps buffer */
	if (ec->curr_pos <= 0)
		ec->curr_pos = ec->taps;
	ec->curr_pos--;

	if (ec->adaption_mode & ECHO_CAN_DISABLE)
		ec->clean_nlp = rx;

	/* Output scaled back up again to match input scaling */

	return (int16_t) ec->clean_nlp << 1;
}

/* This function is seperated from the echo canceller is it is usually called
   as part of the tx process.  See rx HP (DC blocking) filter above, it's
   the same design.

   Some soft phones send speech signals with a lot of low frequency
   energy, e.g. down to 20Hz.  This can make the hybrid non-linear
   which causes the echo canceller to fall over.  This filter can help
   by removing any low frequency before it gets to the tx port of the
   hybrid.

   It can also help by removing and DC in the tx signal.  DC is bad
   for LMS algorithms.

   This is one of the classic DC removal filters, adjusted to provide sufficient
   bass rolloff to meet the above requirement to protect hybrids from things that
   upset them. The difference between successive samples produces a lousy HPF, and
   then a suitably placed pole flattens things out. The final result is a nicely
   rolled off bass end. The filtering is implemented with extended fractional
   precision, which noise shapes things, giving very clean DC removal.
*/

int16_t oslec_hpf_tx(struct oslec_state * ec, int16_t tx)
{
	int tmp, tmp1;

	if (ec->adaption_mode & ECHO_CAN_USE_TX_HPF) {
		tmp = tx << 15;
#if 1
		/* Make sure the gain of the HPF is 1.0. The first can still saturate a little under
		   impulse conditions, and it might roll to 32768 and need clipping on sustained peak
		   level signals. However, the scale of such clipping is small, and the error due to
		   any saturation should not markedly affect the downstream processing. */
		tmp -= (tmp >> 4);
#endif
		ec->tx_1 += -(ec->tx_1 >> DC_LOG2BETA) + tmp - ec->tx_2;
		tmp1 = ec->tx_1 >> 15;
		if (tmp1 > 32767)
			tmp1 = 32767;
		if (tmp1 < -32767)
			tmp1 = -32767;
		tx = tmp1;
		ec->tx_2 = tmp;
	}

	return tx;
}
//////


int main(int argc, char *argv[])
{
    int16_t *rec = NULL;
    int16_t *far = NULL;
    int16_t *out = NULL;
    FILE *fp_rec = NULL;
    FILE *fp_far = NULL;
    FILE *fp_out = NULL;

    int opt = 0;
    int delay = 0;
    int save_audio = 0;
    int daemonize = 0;

    conf_t config = {
        .rec_pcm = "default",
        .out_pcm = "default",
        .playback_fifo = "/tmp/ec.input",
        .out_fifo = "/tmp/ec.output",
        .rate = 16000,
        .rec_channels = 2,
        .ref_channels = 1,
        .out_channels = 2,
        .bits_per_sample = 16,
        .buffer_size = 1024 * 16,
        .playback_fifo_size = 1024 * 4,
        .filter_length = 4096,
        .bypass = 1
    };

    while ((opt = getopt(argc, argv, "b:c:d:Df:hi:o:r:s")) != -1)
    {
        switch (opt)
        {
        case 'b':
            config.buffer_size = atoi(optarg);
            break;
        case 'c':
            config.rec_channels = atoi(optarg);
            config.out_channels = config.rec_channels;
            break;
        case 'd':
            delay = atoi(optarg);
            break;
        case 'D':
            daemonize = 1;
            break;
        case 'f':
            config.filter_length = atoi(optarg);
            break;
        case 'h':
            printf(usage, argv[0]);
            exit(0);
        case 'i':
            config.rec_pcm = optarg;
            break;
        case 'o':
            config.out_pcm = optarg;
            break;
        case 'r':
            config.rate = atoi(optarg);
            break;
        case 's':
            save_audio = 1;
            break;
        case '?':
            printf("\n");
            printf(usage, argv[0]);
            exit(1);
        default:
            break;
        }
    }

    if (daemonize)
    {
        pid_t pid, sid;

        /* Fork off the parent process */
        pid = fork();
        if (pid < 0)
        {
            printf("fork() failed\n");
            exit(1);
        }
        /* If we got a good PID, then
           we can exit the parent process. */
        if (pid > 0)
        {
            exit(0);
        }

        /* Change the file mode mask */
        umask(0);

        /* Open any logs here */

        /* Create a new SID for the child process */
        sid = setsid();
        if (sid < 0)
        {
            printf("setsid() failed\n");
            exit(1);
        }

        /* Change the current working directory */
        if ((chdir("/")) < 0)
        {
            printf("chdir() failed\n");
            exit(1);
        }
    }

    int frame_size = config.rate * 10 / 1000; // 10 ms

    if (save_audio)
    {
        fp_far = fopen("/tmp/playback.raw", "wb");
        fp_rec = fopen("/tmp/recording.raw", "wb");
        fp_out = fopen("/tmp/out.raw", "wb");

        if (fp_far == NULL || fp_rec == NULL || fp_out == NULL)
        {
            printf("Fail to open file(s)\n");
            exit(1);
        }
    }

    rec = (int16_t *)calloc(frame_size * config.rec_channels, sizeof(int16_t));
    far = (int16_t *)calloc(frame_size * config.ref_channels, sizeof(int16_t));
    out = (int16_t *)calloc(frame_size * config.out_channels, sizeof(int16_t));

    if (rec == NULL || far == NULL || out == NULL)
    {
        printf("Fail to allocate memory\n");
        exit(1);
    }

    // Configures signal handling.
    struct sigaction sig_int_handler;
    sig_int_handler.sa_handler = int_handler;
    sigemptyset(&sig_int_handler.sa_mask);
    sig_int_handler.sa_flags = 0;
    sigaction(SIGINT, &sig_int_handler, NULL);
/*
    echo_state = speex_echo_state_init_mc(frame_size,
                                          config.filter_length,
                                          config.rec_channels,
                                          config.ref_channels);
    speex_echo_ctl(echo_state, SPEEX_ECHO_SET_SAMPLING_RATE, &(config.rate));
*/
    oslec = oslec_create(frame_size, ECHO_CAN_USE_ADAPTION | ECHO_CAN_USE_NLP | ECHO_CAN_USE_CLIP | ECHO_CAN_USE_TX_HPF | ECHO_CAN_USE_RX_HPF);
    

    playback_start(&config);
    capture_start(&config);
    fifo_setup(&config);

    printf("Running... Press Ctrl+C to exit\n");

    int timeout = 200 * 1000 * frame_size / config.rate;    // ms

    // system delay between recording and playback
    printf("skip frames %d\n", capture_skip(delay));

    while (!g_is_quit)
    {
        capture_read(rec, frame_size, timeout);
        playback_read(far, frame_size, timeout);

        if (!config.bypass)
        {
            //speex_echo_cancellation(echo_state, rec, far, out);
            *out=oslec_update(oslec, *far, *rec);
        }
        else
        {
            memcpy(out, rec, frame_size * config.rec_channels * config.bits_per_sample / 8);
        }

        if (fp_far)
        {
            fwrite(rec, 2, frame_size * config.rec_channels, fp_rec);
            fwrite(far, 2, frame_size, fp_far);
            fwrite(out, 2, frame_size * config.out_channels, fp_out);
        }

        fifo_write(out, frame_size);
    }

    if (fp_far)
    {
        fclose(fp_rec);
        fclose(fp_far);
        fclose(fp_out);
    }

    free(rec);
    free(far);
    free(out);

    capture_stop();
    playback_stop();

    exit(0);

    return 0;
}
