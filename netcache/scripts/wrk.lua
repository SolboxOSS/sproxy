--counter = 0
filecount	= 300
filesiz 	= 10000000 -- 10MB
maxreq 		= 1024*1024

request    = function()
  	path = "/size/10MB/10MB_" .. math.random(1,filecount) 
	r1 = math.random(0, filesiz-1)
	r2 = math.random(0, filesiz-1)

	if (r2 - r1 < 0)
	then
		tmp = r2
		r2 	= r1
		r1 	= tmp
	end

	if (r2 - r1 > maxreq)
	then
		r2 = r1 + maxreq
	end
	if (r2 >= filesiz)
	then
		r2 = filesiz - 1
	end
--	r2 = math.max(maxreq - 1, r2)

--	print (path)
	wrk.headers["Host"] = "origin.media.com"
	wrk.headers["Connection"] = "Keep-Alive"
 	range =  string.format("bytes=%d-%d", r1, r2)
--	print (range)
	wrk.headers["Range"] = range
	return wrk.format(nil,path)
end
