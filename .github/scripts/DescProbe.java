import java.util.stream.Collectors;

public class DescProbe {
    public static void main(String[] args) throws Exception {
        Process child = new ProcessBuilder("sleep", "30").start();
        Thread.sleep(500);
        ProcessHandle me = ProcessHandle.current();
        System.out.println("process tree: me " + me.pid() + ", child " + child.pid()
                + ", parent " + me.parent().map(ProcessHandle::pid).orElse(-1L));
        System.out.println("  children " + me.children()
                .map(p -> Long.toString(p.pid())).collect(Collectors.joining(",")));
        me.descendants().forEach(p -> System.out.println("  descendant " + p.pid()
                + " parent " + p.parent().map(ProcessHandle::pid).orElse(-1L)
                + " " + p.info().command().orElse("?")));
        long all = ProcessHandle.allProcesses().count();
        long distinct = ProcessHandle.allProcesses().mapToLong(ProcessHandle::pid).distinct().count();
        String nonpositive = ProcessHandle.allProcesses().filter(p -> p.pid() <= 0)
                .map(p -> Long.toString(p.pid())).collect(Collectors.joining(","));
        System.out.println("  allProcesses " + all + ", distinct " + distinct
                + ", pid <= 0 [" + nonpositive + "]");
        child.destroyForcibly();
    }
}
