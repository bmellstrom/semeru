import java.util.Arrays;

public class Example1 {
	public static synchronized void main(String[] args) throws InterruptedException {
		System.out.println("started: " + Arrays.toString(args));
		Example1.class.wait();
		System.out.println("stopped");
	}

	public static synchronized void shutdown() {
		Example1.class.notifyAll();
	}

	public static synchronized void reload() {
		System.out.println("reloaded");
	}
}
