# R-written inputs for v140; all outputs belong to the caller's temporary stem.
stem <- commandArgs(TRUE)[1]
put <- function(name, x) saveRDS(x, paste0(stem, "_", name, ".rds"), compress = FALSE)
x <- setNames(list(data.frame(id = 111L), data.frame(id = 222L), data.frame(id = 333L)),
              c("", "[[1]]", "[[1]]_1"))
stopifnot(x[["[[1]]"]]$id == 222L)
put("objects", x)
put("dates", data.frame(d = structure(c(0, .5), class = "Date", format.stata = "%td")))
put("limits", data.frame(d = structure(c(-2147483647, 0, 2147483647), class = "Date"),
                         i = structure(c(-2147483647L, 0L, 2147483647L), class = "Date"),
                         t = structure(c(0, 86399.9999999, 86399.999999),
                                       class = c("hms", "difftime"), units = "secs")))
put("compact", data.frame(id = 1:3, other = c(7L, 8L, 9L)))
cat("DONE\n")
