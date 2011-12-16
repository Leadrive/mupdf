/*
 * pdfdraw -- command line tool for drawing pdf documents
 */

#include "fitz.h"
#include "mupdf.h"

#ifdef _MSC_VER
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

char *output = NULL;
float resolution = 72;
float rotation = 0;

int showxml = 0;
int showtext = 0;
int showtime = 0;
int showmd5 = 0;
int showoutline = 0;
int savealpha = 0;
int uselist = 1;
int alphabits = 8;
float gamma_value = 1;
int invert = 0;

fz_colorspace *colorspace;
fz_glyph_cache *glyphcache;
char *filename;

struct {
	int count, total;
	int min, max;
	int minpage, maxpage;
} timing;

static void usage(void)
{
	fprintf(stderr,
		"usage: pdfdraw [options] input.pdf [pages]\n"
		"\t-o -\toutput filename (%%d for page number)\n"
		"\t\tsupported formats: pgm, ppm, pam, png, pbm\n"
		"\t-p -\tpassword\n"
		"\t-r -\tresolution in dpi (default: 72)\n"
		"\t-A\tdisable accelerated functions\n"
		"\t-a\tsave alpha channel (only pam and png)\n"
		"\t-b -\tnumber of bits of antialiasing (0 to 8)\n"
		"\t-g\trender in grayscale\n"
		"\t-m\tshow timing information\n"
		"\t-t\tshow text (-tt for xml)\n"
		"\t-x\tshow display list\n"
		"\t-d\tdisable use of display list\n"
		"\t-5\tshow md5 checksums\n"
		"\t-R -\trotate clockwise by given number of degrees\n"
		"\t-G gamma\tgamma correct output\n"
		"\t-I\tinvert output\n"
		"\t-l\tprint outline\n"
		"\tpages\tcomma separated list of ranges\n");
	exit(1);
}

static int gettime(void)
{
	static struct timeval first;
	static int once = 1;
	struct timeval now;
	if (once)
	{
		gettimeofday(&first, NULL);
		once = 0;
	}
	gettimeofday(&now, NULL);
	return (now.tv_sec - first.tv_sec) * 1000 + (now.tv_usec - first.tv_usec) / 1000;
}

static int isrange(char *s)
{
	while (*s)
	{
		if ((*s < '0' || *s > '9') && *s != '-' && *s != ',')
			return 0;
		s++;
	}
	return 1;
}

static void drawpage(pdf_xref *xref, int pagenum)
{
	pdf_page *page;
	fz_display_list *list = NULL;
	fz_device *dev = NULL;
	int start;
	fz_context *ctx = xref->ctx;

	fz_var(list);
	fz_var(dev);

	if (showtime)
	{
		start = gettime();
	}

	fz_try(ctx)
	{
		page = pdf_load_page(xref, pagenum - 1);
	}
	fz_catch(ctx)
	{
		fz_throw(ctx, "cannot load page %d in file '%s'", pagenum, filename);
	}

	if (uselist)
	{
		fz_try(ctx)
		{
			list = fz_new_display_list(ctx);
			dev = fz_new_list_device(ctx, list);
			pdf_run_page(xref, page, dev, fz_identity);
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_free_display_list(ctx, list);
			pdf_free_page(ctx, page);
			fz_throw(ctx, "cannot draw page %d in file '%s'", pagenum, filename);
		}
		fz_free_device(dev);
	}

	if (showxml)
	{
		fz_try(ctx)
		{
			dev = fz_new_trace_device(ctx);
			printf("<page number=\"%d\">\n", pagenum);
			if (list)
				fz_execute_display_list(list, dev, fz_identity, fz_infinite_bbox);
			else
				pdf_run_page(xref, page, dev, fz_identity);
			printf("</page>\n");
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_free_display_list(ctx, list);
			pdf_free_page(ctx, page);
			fz_rethrow(ctx);
		}
		fz_free_device(dev);
	}

	if (showtext)
	{
		fz_text_span *text = NULL;

		fz_var(text);

		fz_try(ctx)
		{
			text = fz_new_text_span(ctx);
			dev = fz_new_text_device(ctx, text);
			if (list)
				fz_execute_display_list(list, dev, fz_identity, fz_infinite_bbox);
			else
				pdf_run_page(xref, page, dev, fz_identity);
			fz_free_device(dev);
			dev = NULL;
			printf("[Page %d]\n", pagenum);
			if (showtext > 1)
				fz_debug_text_span_xml(text);
			else
				fz_debug_text_span(text);
			printf("\n");
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_free_text_span(ctx, text);
			fz_free_display_list(ctx, list);
			pdf_free_page(ctx, page);
			fz_rethrow(ctx);
		}
		fz_free_text_span(ctx, text);
	}

	if (showmd5 || showtime)
		printf("page %s %d", filename, pagenum);

	if (output || showmd5 || showtime)
	{
		float zoom;
		fz_matrix ctm;
		fz_bbox bbox;
		fz_pixmap *pix = NULL;

		fz_var(pix);

		zoom = resolution / 72;
		ctm = fz_translate(0, -page->mediabox.y1);
		ctm = fz_concat(ctm, fz_scale(zoom, -zoom));
		ctm = fz_concat(ctm, fz_rotate(page->rotate));
		ctm = fz_concat(ctm, fz_rotate(rotation));
		bbox = fz_round_rect(fz_transform_rect(ctm, page->mediabox));

		/* TODO: banded rendering and multi-page ppm */

		fz_try(ctx)
		{
			pix = fz_new_pixmap_with_rect(ctx, colorspace, bbox);

			if (savealpha)
				fz_clear_pixmap(pix);
			else
				fz_clear_pixmap_with_color(pix, 255);

			dev = fz_new_draw_device(ctx, glyphcache, pix);
			if (list)
				fz_execute_display_list(list, dev, ctm, bbox);
			else
				pdf_run_page(xref, page, dev, ctm);
			fz_free_device(dev);
			dev = NULL;

			if (invert)
				fz_invert_pixmap(pix);
			if (gamma_value != 1)
				fz_gamma_pixmap(pix, gamma_value);

			if (savealpha)
				fz_unmultiply_pixmap(pix);

			if (output)
			{
				char buf[512];
				sprintf(buf, output, pagenum);
				if (strstr(output, ".pgm") || strstr(output, ".ppm") || strstr(output, ".pnm"))
					fz_write_pnm(ctx, pix, buf);
				else if (strstr(output, ".pam"))
					fz_write_pam(ctx, pix, buf, savealpha);
				else if (strstr(output, ".png"))
					fz_write_png(ctx, pix, buf, savealpha);
				else if (strstr(output, ".pbm")) {
					fz_halftone *ht = fz_get_default_halftone(ctx, 1);
					fz_bitmap *bit = fz_halftone_pixmap(ctx, pix, ht);
					fz_write_pbm(ctx, bit, buf);
					fz_drop_bitmap(ctx, bit);
					fz_drop_halftone(ctx, ht);
				}
			}

			if (showmd5)
			{
				fz_md5 md5;
				unsigned char digest[16];
				int i;

				fz_md5_init(&md5);
				fz_md5_update(&md5, pix->samples, pix->w * pix->h * pix->n);
				fz_md5_final(&md5, digest);

				printf(" ");
				for (i = 0; i < 16; i++)
					printf("%02x", digest[i]);
			}

			fz_drop_pixmap(ctx, pix);
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_drop_pixmap(ctx, pix);
			fz_free_display_list(ctx, list);
			pdf_free_page(ctx, page);
			fz_rethrow(ctx);
		}
	}

	if (list)
		fz_free_display_list(ctx, list);

	pdf_free_page(ctx, page);

	if (showtime)
	{
		int end = gettime();
		int diff = end - start;

		if (diff < timing.min)
		{
			timing.min = diff;
			timing.minpage = pagenum;
		}
		if (diff > timing.max)
		{
			timing.max = diff;
			timing.maxpage = pagenum;
		}
		timing.total += diff;
		timing.count ++;

		printf(" %dms", diff);
	}

	if (showmd5 || showtime)
		printf("\n");

	fz_flush_warnings(ctx);
}

static void drawrange(pdf_xref *xref, char *range)
{
	int page, spage, epage;
	char *spec, *dash;

	spec = fz_strsep(&range, ",");
	while (spec)
	{
		dash = strchr(spec, '-');

		if (dash == spec)
			spage = epage = pdf_count_pages(xref);
		else
			spage = epage = atoi(spec);

		if (dash)
		{
			if (strlen(dash) > 1)
				epage = atoi(dash + 1);
			else
				epage = pdf_count_pages(xref);
		}

		spage = CLAMP(spage, 1, pdf_count_pages(xref));
		epage = CLAMP(epage, 1, pdf_count_pages(xref));

		if (spage < epage)
			for (page = spage; page <= epage; page++)
				drawpage(xref, page);
		else
			for (page = spage; page >= epage; page--)
				drawpage(xref, page);

		spec = fz_strsep(&range, ",");
	}
}

static void drawoutline(pdf_xref *xref)
{
	fz_outline *outline = pdf_load_outline(xref);
	if (showoutline > 1)
		fz_debug_outline_xml(outline, 0);
	else
		fz_debug_outline(outline, 0);
	fz_free_outline(outline);
}

int main(int argc, char **argv)
{
	char *password = "";
	int grayscale = 0;
	int accelerate = 1;
	pdf_xref *xref = NULL;
	int c;
	fz_context *ctx;

	fz_var(xref);

	while ((c = fz_getopt(argc, argv, "lo:p:r:R:Aab:dgmtx5G:I")) != -1)
	{
		switch (c)
		{
		case 'o': output = fz_optarg; break;
		case 'p': password = fz_optarg; break;
		case 'r': resolution = atof(fz_optarg); break;
		case 'R': rotation = atof(fz_optarg); break;
		case 'A': accelerate = 0; break;
		case 'a': savealpha = 1; break;
		case 'b': alphabits = atoi(fz_optarg); break;
		case 'l': showoutline++; break;
		case 'm': showtime++; break;
		case 't': showtext++; break;
		case 'x': showxml++; break;
		case '5': showmd5++; break;
		case 'g': grayscale++; break;
		case 'd': uselist = 0; break;
		case 'G': gamma_value = atof(fz_optarg); break;
		case 'I': invert++; break;
		default: usage(); break;
		}
	}

	if (fz_optind == argc)
		usage();

	if (!showtext && !showxml && !showtime && !showmd5 && !showoutline && !output)
	{
		printf("nothing to do\n");
		exit(0);
	}

	if (accelerate)
		fz_accelerate();

	ctx = fz_new_context(&fz_alloc_default, 256<<20);
	if (!ctx)
	{
		fprintf(stderr, "cannot initialise context\n");
		exit(1);
	}

	fz_set_aa_level(ctx, alphabits);

	colorspace = fz_device_rgb;
	if (grayscale)
		colorspace = fz_device_gray;
	if (output && strstr(output, ".pgm"))
		colorspace = fz_device_gray;
	if (output && strstr(output, ".ppm"))
		colorspace = fz_device_rgb;
	if (output && strstr(output, ".pbm"))
		colorspace = fz_device_gray;

	timing.count = 0;
	timing.total = 0;
	timing.min = 1 << 30;
	timing.max = 0;
	timing.minpage = 0;
	timing.maxpage = 0;

	if (showxml)
		printf("<?xml version=\"1.0\"?>\n");

	fz_try(ctx)
	{
		glyphcache = fz_new_glyph_cache(ctx);

		while (fz_optind < argc)
		{
			filename = argv[fz_optind++];

			fz_try(ctx)
			{
				xref = pdf_open_xref(ctx, filename, password);
			}
			fz_catch(ctx)
			{
				fz_throw(ctx, "cannot open document: %s", filename);
			}

			fz_try(ctx)
			{
				pdf_load_page_tree(xref);
			}
			fz_catch(ctx)
			{
				fz_throw(ctx, "cannot load page tree: %s", filename);
			}

			if (showxml)
				printf("<document name=\"%s\">\n", filename);

			if (showoutline)
				drawoutline(xref);

			if (showtext || showxml || showtime || showmd5 || output)
			{
				if (fz_optind == argc || !isrange(argv[fz_optind]))
					drawrange(xref, "1-");
				if (fz_optind < argc && isrange(argv[fz_optind]))
					drawrange(xref, argv[fz_optind++]);
			}

			if (showxml)
				printf("</document>\n");

			pdf_free_xref(xref);
			xref = NULL;
		}
	}
	fz_catch(ctx)
	{
		pdf_free_xref(xref);
	}

	if (showtime)
	{
		printf("total %dms / %d pages for an average of %dms\n",
			timing.total, timing.count, timing.total / timing.count);
		printf("fastest page %d: %dms\n", timing.minpage, timing.min);
		printf("slowest page %d: %dms\n", timing.maxpage, timing.max);
	}

	fz_free_glyph_cache(ctx, glyphcache);
	fz_flush_warnings(ctx);
	fz_free_context(ctx);
	return 0;
}
