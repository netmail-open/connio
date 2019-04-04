#include<connio.h>
#include<iobuf.h>

EXPORT ConnIOCBResponse _ConnFileReader( FILE *fp, char *buffer, size_t *size )
{
	int		bytes;

	if( fp )
	{
		bytes = fread( buffer, 1, *size, fp );
		if( bytes > 0 )
		{
			*size = bytes;
		}
		else
		{
			*size = 0;
		}
		if( feof( fp ) )
		{
			errno = 0;
			return CONN_IOCBR_DONE;
		}
		if( ferror( fp ) )
		{
			return CONN_IOCBR_ERROR;
		}
		errno = 0;
		return CONN_IOCBR_CONTINUE;
	}
	*size = 0;
	return CONN_IOCBR_ERROR;
}

EXPORT ConnIOCBResponse _ConnFileWriter( FILE *fp, char *buffer, size_t *size )
{
	int		bytes;

	if( fp )
	{
		bytes = fwrite( buffer, 1, *size, fp );
		if( bytes > 0 )
		{
			*size = bytes;
			return CONN_IOCBR_CONTINUE;
		}

		DebugAssert(0);
	}

	DebugPrintf("_ConnFileWriter() called with a size of %zd and an fp of %p.  Returning CONN_IOCBR_ERROR\n", *size, fp);
	*size = 0;
	return CONN_IOCBR_ERROR;
}

EXPORT ConnIOCBResponse _ConnFileWriterEOS( FILE *fp, char *buffer, size_t *size )
{
	int		bytes;
	char	*match;

	if( fp )
	{
		bytes = MemSearch( buffer, *size, "\n.\n", 3, SEARCH_IGNORE_CR|SEARCH_REPLACE_NULL, &match, NULL );
		if( bytes > 0 )
		{
			if( bytes != fwrite( buffer, 1, bytes, fp ) )
			{
				*size = 0;
				return CONN_IOCBR_ERROR;
			}
		}
		*size = bytes;
		if( match )
		{
			if( 5 != fwrite( "\r\n.\r\n", 1, 5, fp ) )
			{
				return CONN_IOCBR_ERROR;
			}
			(*size) += 5;
			return CONN_IOCBR_DONE;
		}
		return (bytes) ? CONN_IOCBR_CONTINUE : CONN_IOCBR_NEED_MORE_DATA;
	}
	*size = 0;
	return CONN_IOCBR_ERROR;
}

