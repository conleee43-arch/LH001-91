for /r %%f in (Objects Listings Output Debug Release Debug settings) do (
    if exist "%%f" (
        echo del: %%f
        rd /s /q "%%f" 2>nul
    )
)

del *.bak /s
del *.ddk /s
del *.edk /s
del *.lst /s
del *.lnp /s
del *.mpf /s
del *.mpj /s
del *.obj /s
del *.omf /s
del *.plg /s
del *.rpt /s
del *.tmp /s
del *.__i /s
del *._2i /s
del *.fed /s
del *.l2p /s
del *.crf /s
del *.o /s
del *.d /s
del *.axf /s
del *.tra /s
del *.dep /s
del JLinkLog.txt /s
del *.iex /s
del *.bin /s
del *.uvguix.* /s
del *.dblite /s
del *.uvgui.* /s
del *.vpwhist /s
del *.vtg /s
del *.vpj /s
del *.vpw /s
del *.hex /s
del *.out /s
del *.sim /s
del *.map /s
del *.browse /s
del *.linf /s
del *.pbd /s
del *.pbi /s
del *.xcl /s
del *.db /s
del *.log /s
del *.uvguix.* /s
del EventRecorderStub.scvd /s
del JLinkSettings.ini /s
del *.dep /s
del *.ewd /s
del *.ewt /s

exit
