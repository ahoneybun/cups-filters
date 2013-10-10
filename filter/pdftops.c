/*
 * "$Id$"
 *
 *   PDF to PostScript filter front-end for CUPS.
 *
 *   Copyright 2007-2011 by Apple Inc.
 *   Copyright 1997-2006 by Easy Software Products.
 *   Copyright 2011-2012 by Till Kamppeter
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Apple Inc. and are protected by Federal copyright
 *   law.  Distribution and use rights are outlined in the file "LICENSE.txt"
 *   which should have been included with this file.  If this file is
 *   file is missing or damaged, see the license at "http://www.cups.org/".
 *
 * Contents:
 *
 *   main()       - Main entry for filter...
 *   cancel_job() - Flag the job as canceled.
 */

/*
 * Include necessary headers...
 */

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/file.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <config.h>
#include <cupsfilters/image-private.h>

#define MAX_CHECK_COMMENT_LINES	20

/*
 * Type definitions
 */

typedef unsigned renderer_t;
enum renderer_e {GS = 0, PDFTOPS = 1, ACROREAD = 2, PDFTOCAIRO = 3, HYBRID = 4};

/*
 * Local functions...
 */

static void		cancel_job(int sig);


/*
 * Local globals...
 */

static int		job_canceled = 0;
int			pdftopdfapplied = 0;
char			deviceCopies[32] = "1";
int			deviceCollate = 0;


/*
 * When calling the "pstops" filter we exclude the following options from its
 * command line as we have applied these options already to the PDF input,
 * either on the "pdftops"/Ghostscript call in this filter or by use of the
 * "pdftopdf" filter before this filter.
 */

const char *pstops_exclude_general[] = {
  "fitplot",
  "fit-to-page",
  "landscape",
  "orientation-requested",
  NULL
};

const char *pstops_exclude_page_management[] = {
  "brightness",
  "Collate",
  "cupsEvenDuplex",
  "gamma",
  "hue",
  "ipp-attribute-fidelity",
  "MirrorPrint",
  "mirror",
  "multiple-document-handling",
  "natural-scaling",
  "number-up",
  "number-up-layout",
  "OutputOrder",
  "page-border",
  "page-bottom",
  "page-label",
  "page-left",
  "page-ranges",
  "page-right",
  "page-set",
  "page-top",
  "position",
  "saturation",
  "scaling",
  NULL
};


/*
 * Check whether we were called after the "pdftopdf" filter and extract
 * parameters passed over by "pdftopdf" in the header comments of the PDF
 * file
 */

static void parsePDFTOPDFComment(char *filename)
{
  char buf[4096];
  int i;
  FILE *fp;

  if ((fp = fopen(filename,"rb")) == 0) {
    fprintf(stderr, "ERROR: pdftops - cannot open print file \"%s\"\n",
            filename);
    return;
  }

  /* skip until PDF start header */
  while (fgets(buf,sizeof(buf),fp) != 0) {
    if (strncmp(buf,"%PDF",4) == 0) {
      break;
    }
  }
  for (i = 0;i < MAX_CHECK_COMMENT_LINES;i++) {
    if (fgets(buf,sizeof(buf),fp) == 0) break;
    if (strncmp(buf,"%%PDFTOPDFNumCopies",19) == 0) {
      char *p;

      p = strchr(buf+19,':') + 1;
      while (*p == ' ' || *p == '\t') p++;
      strncpy(deviceCopies, p, sizeof(deviceCopies));
      deviceCopies[sizeof(deviceCopies) - 1] = '\0';
      p = deviceCopies + strlen(deviceCopies) - 1;
      while (*p == ' ' || *p == '\t'  || *p == '\r'  || *p == '\n') p--;
      *(p + 1) = '\0';
      pdftopdfapplied = 1;
    } else if (strncmp(buf,"%%PDFTOPDFCollate",17) == 0) {
      char *p;

      p = strchr(buf+17,':') + 1;
      while (*p == ' ' || *p == '\t') p++;
      if (strncasecmp(p,"true",4) == 0) {
	deviceCollate = 1;
      } else {
	deviceCollate = 0;
      }
      pdftopdfapplied = 1;
    } else if (strcmp(buf,"% This file was generated by pdftopdf") == 0) {
      pdftopdfapplied = 1;
    }
  }

  fclose(fp);
}


/*
 * Remove all options in option_list from the string option_str, including
 * option values after an "=" sign and preceded "no" before boolean options
 */

void remove_options(char *options_str, const char **option_list)
{
  const char	**option;		/* Option to be removed now */
  char		*option_start,		/* Start of option in string */
		*option_end;		/* End of option in string */

  for (option = option_list; *option; option ++)
  {
    option_start = options_str;

    while ((option_start = strcasestr(option_start, *option)) != NULL)
    {
      if (!option_start[strlen(*option)] ||
          isspace(option_start[strlen(*option)] & 255) ||
          option_start[strlen(*option)] == '=')
      {
        /*
         * Strip option...
         */

        option_end = option_start + strlen(*option);

        /* Remove preceding "no" of boolean option */
        if ((option_start - options_str) >= 2 &&
            !strncasecmp(option_start - 2, "no", 2))
          option_start -= 2;

	/* Is match of the searched option name really at the beginning of
	   the name of the option in the command line? */
	if ((option_start > options_str) &&
	    (!isspace(*(option_start - 1) & 255)))
	{
	  /* Prevent the same option to be found again. */
	  option_start += 1;
	  /* Skip */
	  continue;
	}

        /* Remove "=" and value */
        while (*option_end && !isspace(*option_end & 255))
          option_end ++;

        /* Remove spaces up to next option */
        while (*option_end && isspace(*option_end & 255))
          option_end ++;

        memmove(option_start, option_end, strlen(option_end) + 1);
      } else {
        /* Prevent the same option to be found again. */
        option_start += 1;
      }
    }
  }
}


/*
 * 'main()' - Main entry for filter...
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line args */
     char *argv[])			/* I - Command-line arguments */
{
  renderer_t    renderer = CUPS_PDFTOPS_RENDERER; /* Renderer: gs or pdftops or acroread or pdftocairo or hybrid */
  int		fd = 0;			/* Copy file descriptor */
  char		*filename,		/* PDF file to convert */
		tempfile[1024];		/* Temporary file */
  char		buffer[8192];		/* Copy buffer */
  int		bytes;			/* Bytes copied */
  int		num_options;		/* Number of options */
  cups_option_t	*options;		/* Options */
  const char	*val;			/* Option value */
  int		orientation,		/* Output orientation */
		fit;			/* Fit output to default page size? */
  ppd_file_t	*ppd;			/* PPD file */
  ppd_size_t	*size;			/* Current page size */
  char		resolution[128] = "300";/* Output resolution */
  int           xres = 0, yres = 0,     /* resolution values */
                maxres = CUPS_PDFTOPS_MAX_RESOLUTION,
                                        /* Maximum image rendering resolution */
                numvalues;              /* Number of values actually read */
  ppd_choice_t  *choice;
  ppd_attr_t    *attr;
  cups_page_header2_t header;
  cups_file_t	*fp;			/* Post-processing input file */
  int		pdf_pid,		/* Process ID for pdftops */
		pdf_argc,		/* Number of args for pdftops */
		pstops_pid,		/* Process ID of pstops filter */
		pstops_pipe[2],		/* Pipe to pstops filter */
		need_post_proc = 0,     /* Post-processing needed? */
		post_proc_pid = 0,	/* Process ID of post-processing */
		post_proc_pipe[2],	/* Pipe to post-processing */
		wait_children,		/* Number of child processes left */
		wait_pid,		/* Process ID from wait() */
		wait_status,		/* Status from child */
		exit_status = 0;	/* Exit status */
  char		*pdf_argv[100],		/* Arguments for pdftops/gs */
		pdf_width[255],		/* Paper width */
		pdf_height[255],	/* Paper height */
		pdf_widthxheight[255],	/* Paper width x height */
		pstops_path[1024],	/* Path to pstops program */
		*pstops_argv[7],	/* Arguments for pstops filter */
		*pstops_options,	/* Options for pstops filter */
		*pstops_end;		/* End of pstops filter option */
  const char	*cups_serverbin;	/* CUPS_SERVERBIN environment variable */
  const char	*content_type;		/* CONTENT_TYPE environment variable */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


 /*
  * Make sure status messages are not buffered...
  */

  setbuf(stderr, NULL);

 /*
  * Ignore broken pipe signals...
  */

  signal(SIGPIPE, SIG_IGN);

 /*
  * Make sure we have the right number of arguments for CUPS!
  */

  if (argc < 6 || argc > 7)
  {
    fprintf(stderr, "Usage: %s job user title copies options [file]\n",
	    argv[0]);
    return (1);
  }

 /*
  * Register a signal handler to cleanly cancel a job.
  */

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGTERM, cancel_job);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  action.sa_handler = cancel_job;
  sigaction(SIGTERM, &action, NULL);
#else
  signal(SIGTERM, cancel_job);
#endif /* HAVE_SIGSET */

 /*
  * Copy stdin if needed...
  */

  if (argc == 6)
  {
   /*
    * Copy stdin to a temp file...
    */

    if ((fd = cupsTempFd(tempfile, sizeof(tempfile))) < 0)
    {
      perror("DEBUG: Unable to copy PDF file");
      return (1);
    }

    fprintf(stderr, "DEBUG: pdftops - copying to temp print file \"%s\"\n",
            tempfile);

    while ((bytes = fread(buffer, 1, sizeof(buffer), stdin)) > 0)
      bytes = write(fd, buffer, bytes);

    close(fd);

    filename = tempfile;
  }
  else
  {
   /*
    * Use the filename on the command-line...
    */

    filename    = argv[6];
    tempfile[0] = '\0';
  }

 /*
  * Read out copy counts and collate setting passed over by pdftopdf
  */

  parsePDFTOPDFComment(filename);

 /*
  * Load the PPD file and mark options...
  */

  ppd         = ppdOpenFile(getenv("PPD"));
  num_options = cupsParseOptions(argv[5], 0, &options);

  ppdMarkDefaults(ppd);
  cupsMarkOptions(ppd, num_options, options);

 /*
  * Select the PDF renderer: Ghostscript (gs), Poppler (pdftops),
  * Adobe Reader (arcoread), Poppler with Cairo (pdftocairo), or
  * Hybrid (hybrid, Poppler for Brother, Minolta, and Konica Minolta and
  * Ghostscript otherwise)
  */

  if ((val = cupsGetOption("pdftops-renderer", num_options, options)) != NULL)
  {
    if (strcasecmp(val, "gs") == 0)
      renderer = GS;
    else if (strcasecmp(val, "pdftops") == 0)
      renderer = PDFTOPS;
    else if (strcasecmp(val, "acroread") == 0)
      renderer = ACROREAD;
    else if (strcasecmp(val, "pdftocairo") == 0)
      renderer = PDFTOCAIRO;
    else if (strcasecmp(val, "hybrid") == 0)
      renderer = HYBRID;
    else
      fprintf(stderr,
	      "WARNING: Invalid value for \"pdftops-renderer\": \"%s\"\n", val);
  }

  if (renderer == HYBRID)
  {
    if (ppd && ppd->manufacturer &&
	(!strncasecmp(ppd->manufacturer, "Brother", 7) ||
	 strcasestr(ppd->manufacturer, "Minolta")))
      {
	fprintf(stderr, "DEBUG: Switching to Poppler's pdftops instead of Ghostscript for Brother, Minolta, and Konica Minolta to work around bugs in the printer's PS interpreters\n");
	renderer = PDFTOPS;
      }
    else
      renderer = GS;
  }

 /*
  * Build the pstops command-line...
  */

  if ((cups_serverbin = getenv("CUPS_SERVERBIN")) == NULL)
    cups_serverbin = CUPS_SERVERBIN;

  snprintf(pstops_path, sizeof(pstops_path), "%s/filter/pstops",
           cups_serverbin);

  pstops_options = strdup(argv[5]);

  /*
   * Strip options which "pstops" does not need to apply any more
   */
  remove_options(pstops_options, pstops_exclude_general);
  if (pdftopdfapplied)
    remove_options(pstops_options, pstops_exclude_page_management);

  if (pdftopdfapplied && deviceCollate)
  {
   /*
    * Add collate option to the pstops call if pdftopdf has found out that the
    * printer does hardware collate.
    */

    pstops_options = realloc(pstops_options, strlen(pstops_options) + 9);
    if (!pstops_options) {
      fprintf(stderr, "ERROR: Can't allocate pstops_options\n");
      exit(2);
    }   
    pstops_end = pstops_options + strlen(pstops_options);
    strcpy(pstops_end, " Collate");
  }

  pstops_argv[0] = argv[0];		/* Printer */
  pstops_argv[1] = argv[1];		/* Job */
  pstops_argv[2] = argv[2];		/* User */
  pstops_argv[3] = argv[3];		/* Title */
  if (pdftopdfapplied)
    pstops_argv[4] = deviceCopies;     	/* Copies */
  else
    pstops_argv[4] = argv[4];		/* Copies */
  pstops_argv[5] = pstops_options;	/* Options */
  pstops_argv[6] = NULL;

 /*
  * Build the command-line for the pdftops or gs filter...
  */

  content_type = getenv("CONTENT_TYPE");
  if (renderer == PDFTOPS)
  {
    pdf_argv[0] = (char *)"pdftops";
    pdf_argc    = 1;
  }
  else if (renderer == GS)
  {
    pdf_argv[0] = (char *)"gs";
    pdf_argv[1] = (char *)"-q";
    pdf_argv[2] = (char *)"-dNOPAUSE";
    pdf_argv[3] = (char *)"-dBATCH";
    pdf_argv[4] = (char *)"-dSAFER";
#    ifdef HAVE_GHOSTSCRIPT_PS2WRITE
    pdf_argv[5] = (char *)"-sDEVICE=ps2write";
#    else
    pdf_argv[5] = (char *)"-sDEVICE=pswrite";
#    endif /* HAVE_GHOSTSCRIPT_PS2WRITE */
    pdf_argv[6] = (char *)"-sOUTPUTFILE=%stdout";
    pdf_argc    = 7;
  }
  else if (renderer == PDFTOCAIRO)
  {
    pdf_argv[0] = (char *)"pdftocairo";
    pdf_argv[1] = (char *)"-ps";
    pdf_argc    = 2;
  }
  else
  {
    pdf_argv[0] = (char *)"acroread";
    pdf_argv[1] = (char *)"-toPostScript";
    pdf_argc    = 2;
  }

  if (ppd)
  {
   /*
    * Set language level and TrueType font handling...
    */

    if (ppd->language_level == 1)
    {
      if (renderer == PDFTOPS)
      {
	pdf_argv[pdf_argc++] = (char *)"-level1";
	pdf_argv[pdf_argc++] = (char *)"-noembtt";
      }
      else if (renderer == GS)
	pdf_argv[pdf_argc++] = (char *)"-dLanguageLevel=1";
      else if (renderer == PDFTOCAIRO)
        fprintf(stderr, "WARNING: Level 1 PostScript not supported by pdftocairo.");
      else
        fprintf(stderr, "WARNING: Level 1 PostScript not supported by acroread.");
    }
    else if (ppd->language_level == 2)
    {
      if (renderer == PDFTOPS)
      {
	pdf_argv[pdf_argc++] = (char *)"-level2";
	if (!ppd->ttrasterizer)
	  pdf_argv[pdf_argc++] = (char *)"-noembtt";
      }
      else if (renderer == GS)
	pdf_argv[pdf_argc++] = (char *)"-dLanguageLevel=2";
      else /* PDFTOCAIRO || ACROREAD */
        pdf_argv[pdf_argc++] = (char *)"-level2";
    }
    else
    {
      if (renderer == PDFTOPS)
      {
        /* Do not emit PS Level 3 with Poppler on HP PostScript laser printers
	   as some do not like it. See https://bugs.launchpad.net/bugs/277404.*/
	if (ppd->manufacturer &&
	    (!strncasecmp(ppd->manufacturer, "HP", 2) ||
	     !strncasecmp(ppd->manufacturer, "Hewlett-Packard", 15)) &&
	    (strcasestr(ppd->nickname, "laserjet")))
	  pdf_argv[pdf_argc++] = (char *)"-level2";
	else
	  pdf_argv[pdf_argc++] = (char *)"-level3";
      }
      else if (renderer == GS)
	pdf_argv[pdf_argc++] = (char *)"-dLanguageLevel=3";
      else /* PDFTOCAIRO || ACROREAD */
        pdf_argv[pdf_argc++] = (char *)"-level3";
    }

    if ((val = cupsGetOption("fitplot", num_options, options)) == NULL)
      val = cupsGetOption("fit-to-page", num_options, options);

    if (val && strcasecmp(val, "no") && strcasecmp(val, "off") &&
	strcasecmp(val, "false"))
      fit = 1;
    else
      fit = 0;

   /*
    * Set output page size...
    */

    size = ppdPageSize(ppd, NULL);
    if (size && fit)
    {
     /*
      * Got the size, now get the orientation...
      */

      orientation = 0;

      if ((val = cupsGetOption("landscape", num_options, options)) != NULL)
      {
	if (strcasecmp(val, "no") != 0 && strcasecmp(val, "off") != 0 &&
	    strcasecmp(val, "false") != 0)
	  orientation = 1;
      }
      else if ((val = cupsGetOption("orientation-requested", num_options,
                                    options)) != NULL)
      {
       /*
	* Map IPP orientation values to 0 to 3:
	*
	*   3 = 0 degrees   = 0
	*   4 = 90 degrees  = 1
	*   5 = -90 degrees = 3
	*   6 = 180 degrees = 2
	*/

	orientation = atoi(val) - 3;
	if (orientation >= 2)
	  orientation ^= 1;
      }

      if ((renderer == PDFTOPS) || (renderer == PDFTOCAIRO))
      {
	if (orientation & 1)
	{
	  snprintf(pdf_width, sizeof(pdf_width), "%.0f", size->length);
	  snprintf(pdf_height, sizeof(pdf_height), "%.0f", size->width);
	}
	else
	{
	  snprintf(pdf_width, sizeof(pdf_width), "%.0f", size->width);
	  snprintf(pdf_height, sizeof(pdf_height), "%.0f", size->length);
	}

	pdf_argv[pdf_argc++] = (char *)"-paperw";
	pdf_argv[pdf_argc++] = pdf_width;
	pdf_argv[pdf_argc++] = (char *)"-paperh";
	pdf_argv[pdf_argc++] = pdf_height;
	pdf_argv[pdf_argc++] = (char *)"-expand";

      }
      else if (renderer == GS)
      {
	if (orientation & 1)
	{
	  snprintf(pdf_width, sizeof(pdf_width), "-dDEVICEWIDTHPOINTS=%.0f",
		   size->length);
	  snprintf(pdf_height, sizeof(pdf_height), "-dDEVICEHEIGHTPOINTS=%.0f",
		   size->width);
	}
	else
	{
	  snprintf(pdf_width, sizeof(pdf_width), "-dDEVICEWIDTHPOINTS=%.0f",
		   size->width);
	  snprintf(pdf_height, sizeof(pdf_height), "-dDEVICEHEIGHTPOINTS=%.0f",
		   size->length);
	}

	pdf_argv[pdf_argc++] = pdf_width;
	pdf_argv[pdf_argc++] = pdf_height;
      }
      else
      {
        if (orientation & 1)
          snprintf(pdf_widthxheight, sizeof(pdf_widthxheight), "%.0fx%.0f",
                   size->length, size->width);
        else
          snprintf(pdf_widthxheight, sizeof(pdf_widthxheight), "%.0fx%.0f",
                   size->width, size->length);

        pdf_argv[pdf_argc++] = (char *)"-size";
        pdf_argv[pdf_argc++] = pdf_widthxheight;
      }
    }
#ifdef HAVE_POPPLER_PDFTOPS_WITH_ORIGPAGESIZES
    else if ((renderer == PDFTOPS) || (renderer == PDFTOCAIRO))
    {
     /*
      *  Use the page sizes of the original PDF document, this way documents
      *  which contain pages of different sizes can be printed correctly
      */

      /* Only do this for unprocessed PDF files */
      if (content_type && !strstr (content_type, "/vnd.cups-"))
        pdf_argv[pdf_argc++] = (char *)"-origpagesizes";
    }
#endif /* HAVE_POPPLER_PDFTOPS_WITH_ORIGPAGESIZES */
    else if (renderer == ACROREAD)
    {
     /*
      * Use the page sizes of the original PDF document, this way documents
      * which contain pages of different sizes can be printed correctly
      */
     
      /* Only do this for unprocessed PDF files */
      if (content_type && !strstr (content_type, "/vnd.cups-"))
        pdf_argv[pdf_argc++] = (char *)"-choosePaperByPDFPageSize";
    }

   /*
    * Set output resolution ...
    */

    /* Ignore error exits of cupsRasterInterpretPPD(), if it found a resolution
       setting before erroring it is OK for us */
    cupsRasterInterpretPPD(&header, ppd, num_options, options, NULL);
    /* 100 dpi is default, this means that if we have 100 dpi here this
       method failed to find the printing resolution */
    if (header.HWResolution[0] > 100 && header.HWResolution[1] > 100)
    {
	xres = header.HWResolution[0];
	yres = header.HWResolution[1];
    }
    else if ((choice = ppdFindMarkedChoice(ppd, "Resolution")) != NULL)
      strncpy(resolution, choice->choice, sizeof(resolution));
    else if ((attr = ppdFindAttr(ppd,"DefaultResolution",NULL)) != NULL)
      strncpy(resolution, attr->value, sizeof(resolution));
  }

  resolution[sizeof(resolution)-1] = '\0';
  if ((xres > 0) || (yres > 0) ||
      ((numvalues = sscanf(resolution, "%dx%d", &xres, &yres)) > 0))
  {
    if ((yres > 0) && (xres > yres)) xres = yres;
  }
  else
    xres = 300;

 /*
  * Get the ceiling for the image rendering resolution
  */

  if ((val = cupsGetOption("pdftops-max-image-resolution", num_options, options)) != NULL)
  {
    if ((numvalues = sscanf(val, "%d", &yres)) > 0)
      maxres = yres;
    else
      fprintf(stderr,
	      "WARNING: Invalid value for \"pdftops-max-image-resolution\": \"%s\"\n", val);
  }

 /*
  * Reduce the image rendering resolution to not exceed a given maximum
  * to make processing of jobs by the PDF->PS converter and the printer faster
  *
  * maxres = 0 means no limit
  */

  if (maxres)
    while (xres > maxres)
      xres = xres / 2;

  if ((renderer == PDFTOPS) || (renderer == PDFTOCAIRO))
  {
#ifdef HAVE_POPPLER_PDFTOPS_WITH_RESOLUTION
   /*
    * Set resolution to avoid slow processing by the printer when the
    * resolution of embedded images does not match the printer's resolution
    */
    pdf_argv[pdf_argc++] = (char *)"-r";
    snprintf(resolution, sizeof(resolution), "%d", xres);
    pdf_argv[pdf_argc++] = resolution;
    fprintf(stderr, "DEBUG: Using image rendering resolution %d dpi\n", xres);
#endif /* HAVE_POPPLER_PDFTOPS_WITH_RESOLUTION */
    pdf_argv[pdf_argc++] = filename;
    pdf_argv[pdf_argc++] = (char *)"-";
  }
  else if (renderer == GS)
  {
   /*
    * Set resolution to avoid slow processing by the printer when the
    * resolution of embedded images does not match the printer's resolution
    */
    snprintf(resolution, 127, "-r%d", xres);
    pdf_argv[pdf_argc++] = resolution;
    fprintf(stderr, "DEBUG: Using image rendering resolution %d dpi\n", xres);
   /*
    * PostScript debug mode: If you send a job with "lpr -o psdebug" Ghostscript
    * will not compress the pages, so that the PostScript code can get
    * analysed. This is especially important if a PostScript printer errors or
    * misbehaves on Ghostscript's output.
    * On Kyocera printers we always suppress page compression, to avoid slow
    * processing of raster images.
    */
    val = cupsGetOption("psdebug", num_options, options);
    if ((val && strcasecmp(val, "no") && strcasecmp(val, "off") &&
	 strcasecmp(val, "false")) ||
	(ppd && ppd->manufacturer &&
	 !strncasecmp(ppd->manufacturer, "Kyocera", 7)))
    {
      fprintf(stderr, "DEBUG: Deactivated compression of pages in Ghostscript's PostScript output (\"psdebug\" debug mode or Kyocera printer)\n");
      pdf_argv[pdf_argc++] = (char *)"-dCompressPages=false";
    }
   /*
    * The PostScript interpreters on many printers have bugs which make
    * the interpreter crash, error out, or otherwise misbehave on too
    * heavily compressed input files, especially if code with compressed
    * elements is compressed again. Therefore we reduce compression here.
    */
    pdf_argv[pdf_argc++] = (char *)"-dCompressFonts=false";
    pdf_argv[pdf_argc++] = (char *)"-dNoT3CCITT";
    if (ppd && ppd->manufacturer &&
	!strncasecmp(ppd->manufacturer, "Brother", 7))
    {
      fprintf(stderr, "DEBUG: Deactivation of Ghostscript's image compression for Brother printers to workarounmd PS interpreter bug\n");
      pdf_argv[pdf_argc++] = (char *)"-dEncodeMonoImages=false";
      pdf_argv[pdf_argc++] = (char *)"-dEncodeColorImages=false";
    }
    pdf_argv[pdf_argc++] = (char *)"-dNOINTERPOLATE";
    pdf_argv[pdf_argc++] = (char *)"-c";
    pdf_argv[pdf_argc++] = (char *)"save pop";
    pdf_argv[pdf_argc++] = (char *)"-f";
    pdf_argv[pdf_argc++] = filename;
  }
  /* acroread has to read from stdin */

  pdf_argv[pdf_argc] = NULL;

 /*
  * Do we need post-processing of the PostScript output to work around bugs
  * of the printer's PostScript interpreter?
  */

  if ((renderer == PDFTOPS) || (renderer == PDFTOPS))
    need_post_proc = 0;
  else if (renderer == GS)
    need_post_proc =
      (ppd && ppd->manufacturer &&
       (!strncasecmp(ppd->manufacturer, "Kyocera", 7) ||
	!strncasecmp(ppd->manufacturer, "Brother", 7)) ? 1 : 0);
  else
    need_post_proc = 1;

 /*
  * Execute "pdftops/gs | pstops [ | post-processing ]"...
  */

  if (pipe(pstops_pipe))
  {
    perror("DEBUG: Unable to create pipe for pstops");

    exit_status = 1;
    goto error;
  }

  if (need_post_proc)
  {
    if (pipe(post_proc_pipe))
    {
      perror("DEBUG: Unable to create pipe for post-processing");

      exit_status = 1;
      goto error;
    }
  }

  if ((pdf_pid = fork()) == 0)
  {
   /*
    * Child comes here...
    */

    if (need_post_proc)
    {
      dup2(post_proc_pipe[1], 1);
      close(post_proc_pipe[0]);
      close(post_proc_pipe[1]);
    }
    else
      dup2(pstops_pipe[1], 1);
    close(pstops_pipe[0]);
    close(pstops_pipe[1]);

    if (renderer == PDFTOPS)
    {
      execvp(CUPS_POPPLER_PDFTOPS, pdf_argv);
      perror("DEBUG: Unable to execute pdftops program");
    }
    else if (renderer == GS)
    {
      execvp(CUPS_GHOSTSCRIPT, pdf_argv);
      perror("DEBUG: Unable to execute gs program");
    }
    else if (renderer == PDFTOCAIRO)
    {
      execvp(CUPS_POPPLER_PDFTOCAIRO, pdf_argv);
      perror("DEBUG: Unable to execute pdftocairo program");
    }
    else
    {
      /*
       * use filename as stdin for acroread to force output to stdout
       */

      if ((fd = open(filename, O_RDONLY)))
      {
        dup2(fd, 0);
        close(fd);
      }
     
      execvp(CUPS_ACROREAD, pdf_argv);
      perror("DEBUG: Unable to execute acroread program");
    }

    exit(1);
  }
  else if (pdf_pid < 0)
  {
   /*
    * Unable to fork!
    */

    if (renderer == PDFTOPS)
      perror("DEBUG: Unable to execute pdftops program");
    else if (renderer == GS)
      perror("DEBUG: Unable to execute gs program");
    else if (renderer == PDFTOCAIRO)
      perror("DEBUG: Unable to execute pdftocairo program");
    else
      perror("DEBUG: Unable to execute acroread program");

    exit_status = 1;
    goto error;
  }

  fprintf(stderr, "DEBUG: Started filter %s (PID %d)\n", pdf_argv[0], pdf_pid);

  if (need_post_proc)
  {
    if ((post_proc_pid = fork()) == 0)
    {
     /*
      * Child comes here...
      */

      dup2(post_proc_pipe[0], 0);
      close(post_proc_pipe[0]);
      close(post_proc_pipe[1]);
      dup2(pstops_pipe[1], 1);
      close(pstops_pipe[0]);
      close(pstops_pipe[1]);

      fp = cupsFileStdin();

      if (renderer == ACROREAD)
      {
       /*
        * Set %Title and %For from filter arguments since acroread inserts
        * garbage for these when using -toPostScript
        */

        while ((bytes = cupsFileGetLine(fp, buffer, sizeof(buffer))) > 0 &&
               strncmp(buffer, "%%BeginProlog", 13))
        {
          if (strncmp(buffer, "%%Title", 7) == 0)
            printf("%%%%Title: %s\n", argv[3]);
          else if (strncmp(buffer, "%%For", 5) == 0)
            printf("%%%%For: %s\n", argv[2]);
          else
            printf("%s", buffer);
        }

       /*
	* Copy the rest of the file
	*/
	while ((bytes = cupsFileRead(fp, buffer, sizeof(buffer))) > 0)
	  fwrite(buffer, 1, bytes, stdout);
      }
      else
      {

       /*
	* Copy everything until after initial comments (Prolog section)
	*/
	while ((bytes = cupsFileGetLine(fp, buffer, sizeof(buffer))) > 0 &&
	       strncmp(buffer, "%%BeginProlog", 13) &&
	       strncmp(buffer, "%%EndProlog", 11) &&
	       strncmp(buffer, "%%BeginSetup", 12) &&
	       strncmp(buffer, "%%Page:", 7))
	  printf("%s", buffer);

	if (bytes > 0)
	{
	 /*
	  * Insert PostScript interpreter bug fix code in the beginning of
	  * the Prolog section (before the first active PostScript code)
	  */
	  if (strncmp(buffer, "%%BeginProlog", 13))
	  {
	    /* No Prolog section, create one */
	    fprintf(stderr, "DEBUG: Adding Prolog section for workaround PostScript code\n");
	    puts("%%BeginProlog");
	  }
	  else
	    printf("%s", buffer);

	  if (renderer == GS && ppd && ppd->manufacturer)
	  {

	   /*
	    * Kyocera printers have a bug in their PostScript interpreter
	    * making them crashing on PostScript input data generated by
	    * Ghostscript's "ps2write" output device.
	    *
	    * The problem can be simply worked around by preceding the PostScript
	    * code with some extra bits.
	    *
	    * See https://bugs.launchpad.net/bugs/951627
	    *
	    * In addition, at least some of Kyocera's PostScript printers are
	    * very slow on rendering images which request interpolation. So we
	    * also add some code to eliminate interpolation requests.
	    *
	    * See https://bugs.launchpad.net/bugs/1026974
	    */

	    if (!strncasecmp(ppd->manufacturer, "Kyocera", 7))
	    {
	      fprintf(stderr, "DEBUG: Inserted workaround PostScript code for Kyocera printers\n");
	      puts("% ===== Workaround insertion by pdftops CUPS filter =====");
	      puts("% Kyocera's PostScript interpreter crashes on early name binding,");
	      puts("% so eliminate all \"bind\"s by redifining \"bind\" to no-op");
	      puts("/bind {} bind def");
	      puts("% Some Kyocera printers have an unacceptably slow implementation");
	      puts("% of image interpolation.");
	      puts("/image");
	      puts("{");
	      puts("  dup /Interpolate known");
	      puts("  {");
	      puts("    dup /Interpolate undef");
	      puts("  } if");
	      puts("  systemdict /image get exec");
	      puts("} def");
	      puts("% =====");
	    }

	   /*
	    * Brother printers have a bug in their PostScript interpreter
	    * making them printing one blank page if PostScript input data
	    * generated by Ghostscript's "ps2write" output device is used.
	    *
	    * The problem can be simply worked around by preceding the PostScript
	    * code with some extra bits.
	    *
	    * See https://bugs.launchpad.net/bugs/950713
	    */

	    else if (!strncasecmp(ppd->manufacturer, "Brother", 7))
	    {
	      fprintf(stderr, "DEBUG: Inserted workaround PostScript code for Brother printers\n");
	      puts("% ===== Workaround insertion by pdftops CUPS filter =====");
	      puts("% Brother's PostScript interpreter spits out the current page");
	      puts("% and aborts the job on the \"currenthalftone\" operator, so redefine");
	      puts("% it to null");
	      puts("/currenthalftone {//null} bind def");
	      puts("/orig.sethalftone systemdict /sethalftone get def");
	      puts("/sethalftone {dup //null eq not {//orig.sethalftone}{pop} ifelse} bind def");
	      puts("% =====");
	    }
	  }

	  if (strncmp(buffer, "%%BeginProlog", 13))
	  {
	    /* Close newly created Prolog section */
	    if (strncmp(buffer, "%%EndProlog", 11))
	      puts("%%EndProlog");
	    printf("%s", buffer);
	  }

	 /*
	  * Copy the rest of the file
	  */
	  while ((bytes = cupsFileRead(fp, buffer, sizeof(buffer))) > 0)
	    fwrite(buffer, 1, bytes, stdout);
	}
      }

      exit(0);
    }
    else if (post_proc_pid < 0)
    {
     /*
      * Unable to fork!
      */

      perror("DEBUG: Unable to execute post-processing process");

      exit_status = 1;
      goto error;
    }

    fprintf(stderr, "DEBUG: Started post-processing (PID %d)\n", post_proc_pid);
  }

  if ((pstops_pid = fork()) == 0)
  {
   /*
    * Child comes here...
    */

    if (need_post_proc)
    {
      close(post_proc_pipe[0]);
      close(post_proc_pipe[1]);
    }
    dup2(pstops_pipe[0], 0);
    close(pstops_pipe[0]);
    close(pstops_pipe[1]);

    execv(pstops_path, pstops_argv);
    perror("DEBUG: Unable to execute pstops program");

    exit(1);
  }
  else if (pstops_pid < 0)
  {
   /*
    * Unable to fork!
    */

    perror("DEBUG: Unable to execute pstops program");

    exit_status = 1;
    goto error;
  }

  fprintf(stderr, "DEBUG: Started filter pstops (PID %d)\n", pstops_pid);

  close(pstops_pipe[0]);
  close(pstops_pipe[1]);
  if (need_post_proc)
  {
    close(post_proc_pipe[0]);
    close(post_proc_pipe[1]);
  }

 /*
  * Wait for the child processes to exit...
  */

  wait_children = 2 + need_post_proc;

  while (wait_children > 0)
  {
   /*
    * Wait until we get a valid process ID or the job is canceled...
    */

    while ((wait_pid = wait(&wait_status)) < 0 && errno == EINTR)
    {
      if (job_canceled)
      {
	kill(pdf_pid, SIGTERM);
	if (need_post_proc)
	  kill(post_proc_pid, SIGTERM);
	kill(pstops_pid, SIGTERM);

	job_canceled = 0;
      }
    }

    if (wait_pid < 0)
      break;

    wait_children --;

   /*
    * Report child status...
    */

    if (wait_status)
    {
      if (WIFEXITED(wait_status))
      {
	exit_status = WEXITSTATUS(wait_status);

	fprintf(stderr, "DEBUG: PID %d (%s) stopped with status %d!\n",
		wait_pid,
		wait_pid == pdf_pid ? (renderer == PDFTOPS ? "pdftops" :
                (renderer == PDFTOCAIRO ? "pdftocairo" :
		(renderer == GS ? "gs" : "acroread"))) :
		(wait_pid == pstops_pid ? "pstops" : "Post-processing"),
		exit_status);
      }
      else if (WTERMSIG(wait_status) == SIGTERM)
      {
	fprintf(stderr,
		"DEBUG: PID %d (%s) was terminated normally with signal %d!\n",
		wait_pid,
		wait_pid == pdf_pid ? (renderer == PDFTOPS ? "pdftops" :
                (renderer == PDFTOCAIRO ? "pdftocairo" :
                (renderer == GS ? "gs" : "acroread"))) :
		(wait_pid == pstops_pid ? "pstops" : "Post-processing"),
		exit_status);
      }
      else
      {
	exit_status = WTERMSIG(wait_status);

	fprintf(stderr, "DEBUG: PID %d (%s) crashed on signal %d!\n", wait_pid,
		wait_pid == pdf_pid ? (renderer == PDFTOPS ? "pdftops" :
                (renderer == PDFTOCAIRO ? "pdftocairo" :
                (renderer == GS ? "gs" : "acroread"))) :
		(wait_pid == pstops_pid ? "pstops" : "Post-processing"),
		exit_status);
      }
    }
    else
    {
      fprintf(stderr, "DEBUG: PID %d (%s) exited with no errors.\n", wait_pid,
	      wait_pid == pdf_pid ? (renderer == PDFTOPS ? "pdftops" :
              (renderer == PDFTOCAIRO ? "pdftocairo" :
              (renderer == GS ? "gs" : "acroread"))) :
	      (wait_pid == pstops_pid ? "pstops" : "Post-processing"));
    }
  }

 /*
  * Cleanup and exit...
  */

  error:

  if (tempfile[0])
    unlink(tempfile);

  return (exit_status);
}


/*
 * 'cancel_job()' - Flag the job as canceled.
 */

static void
cancel_job(int sig)			/* I - Signal number (unused) */
{
  (void)sig;

  job_canceled = 1;
}


/*
 * End of "$Id$".
 */
