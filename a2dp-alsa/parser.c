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


//void parseConfigFile(audio *audioCards)
//{
//   char *ch = NULL;
//   char *line = NULL;
//   size_t len = 0;
//   ssize_t read;
//   int sinkNumber = 0,  sourceNumber = 0;

//    debug_print ("read configuration file .........\n");
//    FILE* file = fopen("/var/pcm.conf", "r");
//    if ( 0 != file )
//    {
//        while ((read = getline(&line, &len, file)) != -1)
//        {
//            size_t ln = strlen(line) - 1;
//            if (line[ln] == '\n')
//                line[ln] = '\0';

//            ch = strtok(line, "=");
//            if(strcmp(ch, "sink") == 0)
//            {
//                ch = strtok(NULL, "=");
//                strcpy(audioCards->sinks[sinkNumber].name, ch);
//                audioCards->numOfSinks = sinkNumber;
//                audioCards->busy = 0;
//                sinkNumber++;
//            }
//            else if(strcmp(ch, "source") == 0)
//            {
//                ch = strtok(NULL, "=");
//                strcpy(audioCards->sources[sourceNumber].name, ch);
//                audioCards->numOfSinks = sourceNumber;
//                audioCards->busy = 0;
//                sourceNumber++;
//            }
//        }
//        fclose(file);
//    }
//    else
//    {
//        debug_print("please create config file (/var/pcm.conf)\n");
//    }

//}
