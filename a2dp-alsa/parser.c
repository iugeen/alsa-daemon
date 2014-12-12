#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>

#include <stdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

#include "parser.h"

#define debug_print(...) (fprintf (stderr, __VA_ARGS__))

char sinks[10]= {""};
char sources[10] = {""};

void parseConfigFile()
{
  debug_print ("configuration file ....\n");
  FILE* file = fopen("/var/pcm.conf", "r");
  if ( 0 != file )
  {
    char readinline[4096];
    char* workingstr;
    int sinkNumber = 0;
    int sourceNumber = 0;
    while (0 == feof(file))
    {
      if (0 != fgets(readinline, sizeof(readinline), file))
      {
        workingstr = readinline;
        if ((0 != sizeof(workingstr)) && ('#' != workingstr[0])) // if not a comment
        {
          size_t ln = strlen(workingstr) - 1;
          if (workingstr[ln] == '\n')
              workingstr[ln] = '\0';

          char *ch;
          int addAlsaCard = 0;
          ch = strtok(workingstr, "=");
          while (ch != NULL)
          {
            if(strcmp(ch, "sink") == 0)
            {
              addAlsaCard = 1;
            }
            else if(strcmp(ch, "source") == 0)
            {
              addAlsaCard = 2;
            }
            else if(addAlsaCard == 1)
            {
              debug_print ("SINK : %s\n", ch);
              sinks[sinkNumber] = ch;
              addAlsaCard = 0;
              sinkNumber++;
            }
            else if(addAlsaCard == 2)
            {
              debug_print ("SOURCE : %s\n", ch);
              sources[sourceNumber] = ch;
              addAlsaCard = 0;
              sourceNumber++;
            }

            ch = strtok(NULL, " ,");
          }
        }
      }
    }
    fclose(file);
  }
  else
  {
    debug_print ("no file\n");
  }
}

char* getSinks()
{
  return sinks;
}

char* getSources()
{
  return sources;
}






